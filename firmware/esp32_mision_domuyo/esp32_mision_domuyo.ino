/*
 * ============================================================
 *  Titan ATLAS · Misión Domuyo — Firmware de telemetría
 *  ESP32-S + BMP390 + GPS GY-GPS6MV2 (NEO-6M) + acelerómetro MPU6050 (opcional)
 *  Frecuencia: 2 lecturas por segundo (cada 0,5 s)
 * ============================================================
 *
 *  CONEXIONES (placa ESP32-S de 30 pines)
 *    BMP390:   VCC → 3V3 · GND → GND · SDA → D21 · SCL → D22
 *    GPS:      VCC → 3V3 · GND → GND · TX → RX2/D16 · RX → TX2/D17
 *    MPU6050 (cuando lo tengan): VCC → 3V3 · GND → GND · SDA → D21 · SCL → D22
 *    No usar TX0/RX0 (son del USB).
 *
 *  LIBRERÍAS (Arduino IDE → Herramientas → Administrar bibliotecas)
 *    - Adafruit BMP3XX Library   (instala también Adafruit Unified Sensor y BusIO)
 *    - Adafruit MPU6050
 *    - TinyGPSPlus               (Mikal Hart)
 *    - ArduinoJson               (Benoit Blanchon) v7
 *
 *  PLACA: "ESP32 Dev Module"
 *
 *  ANTES DE COMPILAR: copiar secrets.example.h como secrets.h y completarlo
 *  (red WiFi, URL del servidor y API key). secrets.h no se sube a GitHub.
 *
 *  CÓMO LOGRA 2 LECTURAS POR SEGUNDO
 *    - Lee los sensores cada 0,5 s, independientemente de los envíos.
 *    - Mantiene abierta la conexión HTTPS con el servidor (no la renegocia en cada envío).
 *    - Manda en un solo paquete todo lo pendiente; si se corta el WiFi, guarda
 *      hasta 5 minutos de lecturas y las envía al volver.
 *    - Cada lectura lleva su hora exacta (con milisegundos), así el panel la ubica
 *      bien aunque llegue en un lote.
 *    - Configura el GPS NEO-6M para dar posición 2 veces por segundo.
 */

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BMP3XX.h>
#include <Adafruit_MPU6050.h>
#include <TinyGPSPlus.h>
#include <ArduinoJson.h>
#include <sys/time.h>
#include "secrets.h"

// ---------------------------------------------------------------- CONFIGURACIÓN
#define DEVICE_ID          "titan-atlas-01"
#define SAMPLE_INTERVAL_MS 500    // una lectura cada 0,5 s
#define SEND_INTERVAL_MS   500    // como máximo un envío cada 0,5 s (con todo lo pendiente)
#define IMU_INTERVAL_MS    20     // el acelerómetro se lee a 50 Hz para no perder picos
#define BUFFER_SIZE        600    // lecturas guardadas sin conexión (5 minutos a 2 por segundo)
#define BATCH_SIZE         40     // lecturas máximas por envío

// I2C (BMP390 + MPU6050)
#define I2C_SDA        21
#define I2C_SCL        22
#define SEA_LEVEL_HPA  1013.25    // presión a nivel del mar (ajustar con el dato del día para más precisión)

// GPS GY-GPS6MV2 (NEO-6M)
#define GPS_RX_PIN     16         // al TX del GPS
#define GPS_TX_PIN     17         // al RX del GPS
#define GPS_BAUD       9600
#define GPS_RATE_MS    500        // el GPS calcula la posición cada 0,5 s

// Acelerómetro MPU6050
#define ACCEL_RANGE    MPU6050_RANGE_8_G      // 2, 4, 8 o 16 G según lo que se espere medir
#define GYRO_RANGE     MPU6050_RANGE_500_DEG  // 250, 500, 1000 o 2000 °/s

const float G = 9.80665f;

// ---------------------------------------------------------------- OBJETOS
Adafruit_BMP3XX bmp;
Adafruit_MPU6050 mpu;
TinyGPSPlus gps;
HardwareSerial GPSSerial(2);
WiFiClientSecure tls;
HTTPClient http;

bool bmpOk = false;
bool mpuOk = false;
float groundAltitude = NAN;   // altura al encender, para calcular alt_rel

// Estado del acelerómetro (se actualiza a 50 Hz)
struct ImuState {
  float ax, ay, az;     // m/s²
  float gx, gy, gz;     // rad/s
  float gMax;           // pico de g desde la última lectura
} imu = {NAN, NAN, NAN, NAN, NAN, NAN, 0};

// ---------------------------------------------------------------- LECTURAS
struct Reading {
  uint32_t seq;
  uint64_t tsMs;        // hora de la medición en milisegundos (0 si todavía no hay hora)
  float temperature, pressure, altitude, altRel;
  float accelX, accelY, accelZ, gForce, gMax, pitch, roll, rotation;
  double latitude, longitude;
  float gpsAltitude, speed, hdop;
  int   satellites;
  int   rssi;
  uint32_t uptime;
};

Reading buffer[BUFFER_SIZE];
int bufHead = 0, bufCount = 0;
uint32_t seq = 0;
uint32_t lastSample = 0, lastSend = 0, lastImu = 0;

// ---------------------------------------------------------------- GPS: configuración UBX
void sendUbx(const uint8_t *msg, size_t len) {
  uint8_t a = 0, b = 0;                       // checksum Fletcher de UBX
  for (size_t i = 0; i < len; i++) { a += msg[i]; b += a; }
  GPSSerial.write(0xB5);
  GPSSerial.write(0x62);
  GPSSerial.write(msg, len);
  GPSSerial.write(a);
  GPSSerial.write(b);
  GPSSerial.flush();
}

void configureGps() {
  // Apaga los mensajes NMEA que no se usan (GLL, GSA, GSV, VTG) para que entren
  // 2 posiciones por segundo a 9600 baudios. Quedan GGA y RMC, que usa TinyGPSPlus.
  const uint8_t off[][2] = {{0xF0, 0x01}, {0xF0, 0x02}, {0xF0, 0x03}, {0xF0, 0x05}};
  for (auto &m : off) {
    uint8_t msg[] = {0x06, 0x01, 0x03, 0x00, m[0], m[1], 0x00};   // CFG-MSG
    sendUbx(msg, sizeof(msg));
    delay(30);
  }
  // CFG-RATE: medición cada GPS_RATE_MS
  uint8_t rate[] = {0x06, 0x08, 0x06, 0x00,
                    (uint8_t)(GPS_RATE_MS & 0xFF), (uint8_t)(GPS_RATE_MS >> 8),
                    0x01, 0x00,   // 1 medición por solución
                    0x01, 0x00};  // referencia de tiempo GPS
  sendUbx(rate, sizeof(rate));
}

// ---------------------------------------------------------------- HORA
// Hora en milisegundos: la del reloj interno (sincronizado por internet o por el GPS)
uint64_t nowMs() {
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  if (tv.tv_sec < 1700000000) return 0;   // todavía sin hora válida
  return (uint64_t)tv.tv_sec * 1000ULL + tv.tv_usec / 1000;
}

// Si no hay internet pero el GPS tiene hora, se usa para poner en hora el reloj interno
void syncClockFromGps() {
  static uint32_t lastSync = 0;
  if (nowMs() && millis() - lastSync < 600000) return;   // ya en hora: re-sincroniza cada 10 min
  if (!gps.date.isValid() || !gps.time.isValid() || gps.date.year() < 2024 || gps.time.age() > 500) return;
  struct tm t = {};
  t.tm_year = gps.date.year() - 1900;
  t.tm_mon  = gps.date.month() - 1;
  t.tm_mday = gps.date.day();
  t.tm_hour = gps.time.hour();
  t.tm_min  = gps.time.minute();
  t.tm_sec  = gps.time.second();
  struct timeval tv = {mktime(&t), (suseconds_t)(gps.time.centisecond() * 10000)};
  settimeofday(&tv, nullptr);
  lastSync = millis();
}

// ---------------------------------------------------------------- SENSORES
void readImu() {
  if (!mpuOk) return;
  sensors_event_t a, g, t;
  if (!mpu.getEvent(&a, &g, &t)) return;
  imu.ax = a.acceleration.x;
  imu.ay = a.acceleration.y;
  imu.az = a.acceleration.z;
  imu.gx = g.gyro.x;
  imu.gy = g.gyro.y;
  imu.gz = g.gyro.z;
  float gNow = sqrtf(imu.ax * imu.ax + imu.ay * imu.ay + imu.az * imu.az) / G;
  if (gNow > imu.gMax) imu.gMax = gNow;
}

Reading sample() {
  Reading r;
  r.seq = seq++;
  r.tsMs = nowMs();
  r.temperature = r.pressure = r.altitude = r.altRel = NAN;
  r.accelX = r.accelY = r.accelZ = r.gForce = r.gMax = r.pitch = r.roll = r.rotation = NAN;
  r.latitude = r.longitude = NAN;
  r.gpsAltitude = r.speed = r.hdop = NAN;
  r.satellites = -1;

  // BMP390
  if (bmpOk && bmp.performReading()) {
    r.temperature = bmp.temperature;
    r.pressure    = bmp.pressure / 100.0f;  // Pa → hPa
    r.altitude    = bmp.readAltitude(SEA_LEVEL_HPA);
    if (isnan(groundAltitude)) groundAltitude = r.altitude;
    r.altRel = r.altitude - groundAltitude;
  }

  // MPU6050 (solo si está conectado)
  if (mpuOk && !isnan(imu.ax)) {
    r.accelX = imu.ax;
    r.accelY = imu.ay;
    r.accelZ = imu.az;
    r.gForce = sqrtf(imu.ax * imu.ax + imu.ay * imu.ay + imu.az * imu.az) / G;
    r.gMax   = imu.gMax;
    // Inclinación a partir de la gravedad (válida cuando no hay aceleraciones bruscas)
    r.pitch = atan2f(-imu.ax, sqrtf(imu.ay * imu.ay + imu.az * imu.az)) * 180.0f / PI;
    r.roll  = atan2f(imu.ay, imu.az) * 180.0f / PI;
    r.rotation = sqrtf(imu.gx * imu.gx + imu.gy * imu.gy + imu.gz * imu.gz) * 180.0f / PI;
    imu.gMax = r.gForce;   // reinicia el pico para el próximo intervalo
  }

  // GPS
  if (gps.location.isValid() && gps.location.age() < 2000) {
    r.latitude  = gps.location.lat();
    r.longitude = gps.location.lng();
  }
  if (gps.altitude.isValid() && gps.altitude.age() < 2000) r.gpsAltitude = gps.altitude.meters();
  if (gps.speed.isValid() && gps.speed.age() < 2000)       r.speed = gps.speed.kmph();
  if (gps.satellites.isValid())                            r.satellites = gps.satellites.value();
  if (gps.hdop.isValid() && gps.hdop.value() > 0)          r.hdop = gps.hdop.hdop();

  r.rssi   = WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0;
  r.uptime = millis() / 1000;
  return r;
}

// ---------------------------------------------------------------- BUFFER
void pushBuffer(const Reading &r) {
  if (bufCount == BUFFER_SIZE) {             // lleno: se descarta la más vieja
    bufHead = (bufHead + 1) % BUFFER_SIZE;
    bufCount--;
  }
  buffer[(bufHead + bufCount) % BUFFER_SIZE] = r;
  bufCount++;
}

void addIf(JsonObject o, const char *k, double v) {
  if (!isnan(v)) o[k] = v;
}

void toJson(JsonObject o, const Reading &r) {
  o["device_id"] = DEVICE_ID;
  o["seq"] = r.seq;
  if (r.tsMs) o["ts"] = r.tsMs;   // milisegundos: el servidor ubica cada lectura en su momento exacto
  addIf(o, "temperature", r.temperature);
  addIf(o, "pressure", r.pressure);
  addIf(o, "altitude", r.altitude);
  addIf(o, "alt_rel", r.altRel);
  addIf(o, "accel_x", r.accelX);
  addIf(o, "accel_y", r.accelY);
  addIf(o, "accel_z", r.accelZ);
  addIf(o, "g_force", r.gForce);
  addIf(o, "g_max", r.gMax);
  addIf(o, "pitch", r.pitch);
  addIf(o, "roll", r.roll);
  addIf(o, "rotation", r.rotation);
  addIf(o, "latitude", r.latitude);
  addIf(o, "longitude", r.longitude);
  addIf(o, "gps_altitude", r.gpsAltitude);
  addIf(o, "speed", r.speed);
  addIf(o, "hdop", r.hdop);
  if (r.satellites >= 0) o["satellites"] = r.satellites;
  if (r.rssi) o["rssi"] = r.rssi;
  o["uptime_s"] = r.uptime;
}

// ---------------------------------------------------------------- RED
void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  static uint32_t lastTry = 0;
  if (lastTry && millis() - lastTry < 10000) return;   // reintenta cada 10 s sin bloquear
  lastTry = millis();
  Serial.printf("Conectando a WiFi \"%s\"...\n", WIFI_SSID);
  WiFi.disconnect();
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
}

void onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
    Serial.printf("WiFi conectado · IP %s · señal %d dBm\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());
    configTime(0, 0, "pool.ntp.org", "time.google.com");
  } else if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
    uint8_t r = info.wifi_sta_disconnected.reason;
    const char *why =
      r == 201 ? "no se encuentra la red (¿nombre mal escrito o es de 5 GHz?)" :
      (r == 202 || r == 15 || r == 204) ? "contraseña incorrecta" :
      r == 203 ? "la red rechazó la conexión" :
      r == 200 ? "señal muy débil (acercar el ESP32)" :
      (r == 8 || r == 36) ? "reintentando conexión" : "otro motivo";
    Serial.printf("WiFi desconectado · motivo %u: %s\n", r, why);
  }
}

bool flush() {
  if (!bufCount || WiFi.status() != WL_CONNECTED) return false;

  int n = min(bufCount, BATCH_SIZE);
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  for (int i = 0; i < n; i++) toJson(arr.add<JsonObject>(), buffer[(bufHead + i) % BUFFER_SIZE]);
  String body;
  serializeJson(doc, body);

  // La conexión queda abierta entre envíos (setReuse), así cada envío tarda ~100-200 ms
  if (!http.begin(tls, SERVER_URL)) return false;
  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-API-Key", API_KEY);
  uint32_t t0 = millis();
  int code = http.POST(body);
  String resp = code > 0 ? http.getString() : "";
  http.end();

  if (code == 201) {
    bufHead = (bufHead + n) % BUFFER_SIZE;
    bufCount -= n;
    Serial.printf("  ✔ %d enviada(s) en %lu ms · pendientes: %d\n", n, millis() - t0, bufCount);
    return true;
  }

  if (code < 0) {
    // Diagnóstico: por qué no se pudo conectar al servidor
    char err[120] = "";
    tls.lastError(err, sizeof(err));
    IPAddress ip;
    bool dns = WiFi.hostByName("mision-domuyo.up.railway.app", ip);
    Serial.printf("  ✘ No se pudo conectar: %s · TLS: %s · DNS: %s · señal %d dBm\n",
                  HTTPClient::errorToString(code).c_str(), err,
                  dns ? ip.toString().c_str() : "FALLÓ", WiFi.RSSI());
    tls.stop();   // la próxima vez abre una conexión nueva
  } else {
    Serial.printf("  ✘ HTTP %d: %s\n", code, resp.c_str());
  }
  if (code == 400) {            // datos inválidos: se descartan para no trabar la cola
    bufHead = (bufHead + n) % BUFFER_SIZE;
    bufCount -= n;
  }
  return false;
}

// ---------------------------------------------------------------- SETUP / LOOP
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n== Titan ATLAS · Misión Domuyo ==");
  Serial.println("Firmware v2.0 · 2 lecturas por segundo");
  setenv("TZ", "UTC0", 1);
  tzset();

  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(400000);

  // BMP390 (a 50 Hz internos, con filtro suave para lecturas estables a 2 Hz)
  bmpOk = bmp.begin_I2C(0x77) || bmp.begin_I2C(0x76);
  if (bmpOk) {
    bmp.setTemperatureOversampling(BMP3_OVERSAMPLING_2X);
    bmp.setPressureOversampling(BMP3_OVERSAMPLING_8X);
    bmp.setIIRFilterCoeff(BMP3_IIR_FILTER_COEFF_3);
    bmp.setOutputDataRate(BMP3_ODR_50_HZ);
    bmp.performReading();      // la primera lectura suele ser inexacta
    Serial.println("BMP390 OK");
  } else {
    Serial.println("⚠ BMP390 no encontrado: revisar SDA/SCL y alimentación");
  }

  // MPU6050 (opcional)
  mpuOk = mpu.begin(0x68) || mpu.begin(0x69);
  if (mpuOk) {
    mpu.setAccelerometerRange(ACCEL_RANGE);
    mpu.setGyroRange(GYRO_RANGE);
    mpu.setFilterBandwidth(MPU6050_BAND_21_HZ);
    Serial.println("MPU6050 OK");
  } else {
    Serial.println("Acelerómetro no conectado (se omite)");
  }

  // GPS a 2 Hz
  GPSSerial.begin(GPS_BAUD, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
  delay(100);
  configureGps();
  Serial.println("GPS iniciado a 2 Hz (el primer fix puede tardar 1-15 min a cielo abierto)");

  // WiFi + HTTPS con conexión persistente
  tls.setInsecure();           // para validar el certificado: tls.setCACert(ROOT_CA)
  http.setReuse(true);
  http.setTimeout(5000);
  WiFi.mode(WIFI_STA);
  WiFi.setTxPower(WIFI_POWER_8_5dBm);   // menos potencia = menos consumo (evita el brownout)
  delay(200);
  WiFi.setAutoReconnect(true);
  WiFi.onEvent(onWiFiEvent);
  connectWiFi();
}

void loop() {
  // El GPS manda datos todo el tiempo: hay que leerlos siempre
  while (GPSSerial.available()) gps.encode(GPSSerial.read());
  syncClockFromGps();

  uint32_t now = millis();

  // Acelerómetro a 50 Hz para registrar los picos de G
  if (now - lastImu >= IMU_INTERVAL_MS) {
    lastImu = now;
    readImu();
  }

  // Una lectura cada 0,5 s, a ritmo fijo
  if (now - lastSample >= SAMPLE_INTERVAL_MS) {
    lastSample = (now - lastSample > 2 * SAMPLE_INTERVAL_MS) ? now : lastSample + SAMPLE_INTERVAL_MS;
    Reading r = sample();
    pushBuffer(r);

    Serial.printf("#%u  T=%.2f°C  P=%.2f hPa  Alt=%.1f m (rel %.1f)", r.seq, r.temperature, r.pressure, r.altitude, r.altRel);
    if (mpuOk) Serial.printf("  G=%.2f (máx %.2f)  incl=%.0f°/%.0f°", r.gForce, r.gMax, r.pitch, r.roll);
    // Diagnóstico del GPS: distingue "no conectado" de "conectado buscando satélites"
    if (gps.charsProcessed() < 10)
      Serial.println("  GPS: NO RECIBE DATOS -> revisar TX del GPS en RX2/D16, VCC y GND");
    else if (gps.passedChecksum() == 0)
      Serial.println("  GPS: llegan datos pero ilegibles -> revisar velocidad (9600) o cable RX/TX");
    else if (isnan(r.latitude))
      Serial.printf("  GPS: conectado OK, buscando satelites (%d a la vista)\n", max(r.satellites, 0));
    else
      Serial.printf("  GPS: %.6f, %.6f (%d satelites)\n", r.latitude, r.longitude, r.satellites);
  }

  // Envío: como máximo cada 0,5 s, con todo lo que esté pendiente
  if (bufCount && now - lastSend >= SEND_INTERVAL_MS) {
    lastSend = now;
    connectWiFi();
    flush();
  }
}
