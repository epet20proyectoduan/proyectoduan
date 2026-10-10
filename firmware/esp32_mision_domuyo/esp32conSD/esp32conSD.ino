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
 *    MPU6050:  VCC → 3V3 · GND → GND · SDA → D21 · SCL → D22
 *    SD:       SCK → D18 · MISO/DO → D19 · MOSI/DI → D23 · CS → D27
 *
 *    No usar TX0/RX0 (son del USB).
 *
 *  LIBRERÍAS (Arduino IDE → Herramientas → Administrar bibliotecas)
 *    - Adafruit BMP3XX Library
 *    - Adafruit Unified Sensor y Adafruit BusIO
 *    - Adafruit MPU6050
 *    - TinyGPSPlus (Mikal Hart)
 *    - ArduinoJson (Benoit Blanchon) v7
      - ESP32Servo (Kevin Harrington) 3.2.1
 *
 *  PLACA INSTALADA: "esp32" Espressif Systems V3.3.12
 *
 *  ANTES DE COMPILAR: copiar secrets.example.h como secrets.h y completarlo
 *  (red WiFi, URL del servidor y API key). secrets.h no se sube a GitHub.
 *
 *  REGISTRO EN SD
 *    - Archivo: salidas.csv
 *    - Encabezado general: se escribe solo si el archivo está vacío.
 *    - Fecha: una vez por ejecución, usando exclusivamente la hora sincronizada por NTP.
 *    - Datos: una línea por lectura, con el formato nombre:valor.
 *    - Las lecturas no se guardan en SD hasta disponer de una fecha NTP válida.
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
#include <SPI.h>
#include <SD.h>
#include <ESP32Servo.h>
#include <esp_sntp.h>
#include "secrets.h"

// ---------------------------------------------------------------- CONFIGURACIÓN
#define DEVICE_ID          "titan-atlas-01"
#define SAMPLE_INTERVAL_MS 500
#define SEND_INTERVAL_MS   500
#define IMU_INTERVAL_MS    20
#define BUFFER_SIZE        600
#define BATCH_SIZE         40

// I2C (BMP390 + MPU6050)
#define I2C_SDA        21
#define I2C_SCL        22
#define SEA_LEVEL_HPA  1013.25

// GPS GY-GPS6MV2 (NEO-6M)
#define GPS_RX_PIN     16
#define GPS_TX_PIN     17
#define GPS_BAUD       9600
#define GPS_RATE_MS    500

// Acelerómetro MPU6050
#define ACCEL_RANGE    MPU6050_RANGE_8_G
#define GYRO_RANGE     MPU6050_RANGE_500_DEG

const float G = 9.80665f;

// Pines SPI de la tarjeta SD
#define SD_SCK_PIN   18
#define SD_MISO_PIN  19
#define SD_MOSI_PIN  23
#define SD_CS_PIN    27

// Archivo de registro
#define SD_FILE "/salidas.csv"

// ---------------------------------------------------------------- OBJETOS
Adafruit_BMP3XX bmp;
Adafruit_MPU6050 mpu;
TinyGPSPlus gps;
HardwareSerial GPSSerial(2);
WiFiClientSecure tls;
HTTPClient http;

// Estado de la tarjeta SD y de la sincronización horaria
bool sdOk = false;
bool ntpTimeValid = false;
bool dateHeaderWritten = false;

bool bmpOk = false;
bool mpuOk = false;
float groundAltitude = NAN;

// Estado del acelerómetro (se actualiza a 50 Hz)
struct ImuState {
  float ax, ay, az;
  float gx, gy, gz;
  float gMax;
} imu = {NAN, NAN, NAN, NAN, NAN, NAN, 0};

// ---------------------------------------------------------------- LECTURAS
struct Reading {
  uint32_t seq;
  uint64_t tsMs;
  float temperature, pressure, altitude, altRel;
  float accelX, accelY, accelZ, gForce, gMax, pitch, roll, rotation;
  double latitude, longitude;
  float gpsAltitude, speed, hdop;
  int satellites;
  int rssi;
  uint32_t uptime;
};

Reading buffer[BUFFER_SIZE];
int bufHead = 0, bufCount = 0;
uint32_t seq = 0;
uint32_t lastSample = 0, lastSend = 0, lastImu = 0;

// ---------------------------------------------------------------- GPS: CONFIGURACIÓN UBX
void sendUbx(const uint8_t *msg, size_t len) {
  uint8_t a = 0, b = 0;

  for (size_t i = 0; i < len; i++) {
    a += msg[i];
    b += a;
  }

  GPSSerial.write(0xB5);
  GPSSerial.write(0x62);
  GPSSerial.write(msg, len);
  GPSSerial.write(a);
  GPSSerial.write(b);
  GPSSerial.flush();
}

void configureGps() {
  // Apaga los mensajes NMEA que no se usan para mantener 2 posiciones por segundo.
  const uint8_t off[][2] = {
    {0xF0, 0x01},
    {0xF0, 0x02},
    {0xF0, 0x03},
    {0xF0, 0x05}
  };

  for (auto &m : off) {
    uint8_t msg[] = {0x06, 0x01, 0x03, 0x00, m[0], m[1], 0x00};
    sendUbx(msg, sizeof(msg));
    delay(30);
  }

  // Configura el intervalo de medición del GPS.
  uint8_t rate[] = {
    0x06, 0x08, 0x06, 0x00,
    (uint8_t)(GPS_RATE_MS & 0xFF),
    (uint8_t)(GPS_RATE_MS >> 8),
    0x01, 0x00,
    0x01, 0x00
  };

  sendUbx(rate, sizeof(rate));
}

// ---------------------------------------------------------------- HORA

// Devuelve la hora del reloj interno en milisegundos si está disponible.
uint64_t nowMs() {
  struct timeval tv;
  gettimeofday(&tv, nullptr);

  if (tv.tv_sec < 1700000000) return 0;

  return (uint64_t)tv.tv_sec * 1000ULL + tv.tv_usec / 1000;
}

// Se ejecuta cuando el reloj recibe una sincronización horaria por NTP.
void onNtpSync(struct timeval *tv) {
  ntpTimeValid = true;
  Serial.println("Hora sincronizada por NTP");
}

// Si todavía no hay hora NTP, permite sincronizar el reloj interno desde el GPS.
// La fecha registrada en la SD no utiliza esta sincronización.
void syncClockFromGps() {
  static uint32_t lastSync = 0;

  if (ntpTimeValid) return;
  if (nowMs() && millis() - lastSync < 600000) return;

  if (!gps.date.isValid() || !gps.time.isValid() ||
      gps.date.year() < 2024 || gps.time.age() > 500) {
    return;
  }

  struct tm t = {};
  t.tm_year = gps.date.year() - 1900;
  t.tm_mon = gps.date.month() - 1;
  t.tm_mday = gps.date.day();
  t.tm_hour = gps.time.hour();
  t.tm_min = gps.time.minute();
  t.tm_sec = gps.time.second();

  struct timeval tv = {
    mktime(&t),
    (suseconds_t)(gps.time.centisecond() * 10000)
  };

  settimeofday(&tv, nullptr);
  lastSync = millis();
}

// Devuelve la fecha UTC únicamente si NTP ya sincronizó el reloj.
bool getNtpDate(char *dateText, size_t dateSize) {
  if (!ntpTimeValid) return false;

  struct timeval tv;
  gettimeofday(&tv, nullptr);

  if (tv.tv_sec < 1700000000) return false;

  time_t seconds = tv.tv_sec;
  struct tm timeInfo = {};

  if (gmtime_r(&seconds, &timeInfo) == nullptr) return false;

  strftime(dateText, dateSize, "%Y-%m-%d", &timeInfo);
  return true;
}

// ---------------------------------------------------------------- SD: REGISTRO DE DATOS

// Escribe un valor numérico y deja el campo vacío si no hay una medición válida.
void printNamedFloat(File &file, const char *name, float value, uint8_t decimals = 2) {
  file.print(name);
  file.print(':');

  if (!isnan(value) && !isinf(value)) {
    file.print(value, decimals);
  }
}

// Escribe un valor numérico de doble precisión.
void printNamedDouble(File &file, const char *name, double value, uint8_t decimals = 6) {
  file.print(name);
  file.print(':');

  if (!isnan(value) && !isinf(value)) {
    file.print(value, decimals);
  }
}

// Inicializa la tarjeta SD y prepara el archivo.
void initSD() {
  SPI.begin(SD_SCK_PIN, SD_MISO_PIN, SD_MOSI_PIN, SD_CS_PIN);

  if (!SD.begin(SD_CS_PIN, SPI)) {
    Serial.println("Error: no se pudo inicializar la tarjeta SD");
    sdOk = false;
    return;
  }

  sdOk = true;
  Serial.println("Tarjeta SD inicializada");

  File file = SD.open(SD_FILE, FILE_APPEND);

  if (!file) {
    Serial.println("Error: no se pudo abrir salidas.csv");
    sdOk = false;
    return;
  }

  // El encabezado general se escribe solamente si el archivo está vacío.
  if (file.size() == 0) {
    file.println("# Titan ATLAS - Mision Domuyo");
    file.flush();
  }

  file.close();
}

// Guarda una lectura en salidas.csv.
void saveToSD(const Reading &r) {
  if (!sdOk) return;

  // No se registra la fecha ni se guardan muestras hasta tener hora NTP válida.
  // La hora del GPS no se utiliza para fechar el archivo SD.
  char dateText[11];

  if (!getNtpDate(dateText, sizeof(dateText))) return;

  File file = SD.open(SD_FILE, FILE_APPEND);

  if (!file) {
    Serial.println("Error: no se pudo abrir salidas.csv para guardar");
    return;
  }

  // Imprime la fecha una sola vez durante cada ejecución del firmware.
  if (!dateHeaderWritten) {
    file.print("# Fecha: ");
    file.println(dateText);
    file.flush();
    dateHeaderWritten = true;
  }

  // Registra los campos seleccionados en el formato nombre:valor.
  file.print("device_id:");
  file.print(DEVICE_ID);

  file.print(", seq:");
  file.print(r.seq);

  file.print(", ");
  printNamedFloat(file, "temperature", r.temperature, 2);

  file.print(", ");
  printNamedFloat(file, "pressure", r.pressure, 2);

  file.print(", ");
  printNamedFloat(file, "altitude", r.altitude, 2);

  file.print(", ");
  printNamedFloat(file, "alt_rel", r.altRel, 2);

  file.print(", ");
  printNamedFloat(file, "accel_x", r.accelX, 4);

  file.print(", ");
  printNamedFloat(file, "accel_y", r.accelY, 4);

  file.print(", ");
  printNamedFloat(file, "accel_z", r.accelZ, 4);

  file.print(", ");
  printNamedFloat(file, "g_max", r.gMax, 4);

  file.print(", ");
  printNamedFloat(file, "rotation", r.rotation, 2);

  file.print(", ");
  printNamedDouble(file, "latitude", r.latitude, 6);

  file.print(", ");
  printNamedDouble(file, "longitude", r.longitude, 6);

  file.print(", ");
  printNamedFloat(file, "gps_altitude", r.gpsAltitude, 2);

  file.print(", ");
  printNamedFloat(file, "speed", r.speed, 2);

  file.print(", rssi:");
  file.print(r.rssi);

  file.print(", uptime_s:");
  file.println(r.uptime);

  file.flush();
  file.close();
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

  float gNow = sqrtf(
    imu.ax * imu.ax +
    imu.ay * imu.ay +
    imu.az * imu.az
  ) / G;

  if (gNow > imu.gMax) imu.gMax = gNow;
}

Reading sample() {
  Reading r;

  r.seq = seq++;
  r.tsMs = nowMs();

  r.temperature = r.pressure = r.altitude = r.altRel = NAN;
  r.accelX = r.accelY = r.accelZ = r.gForce = r.gMax = NAN;
  r.pitch = r.roll = r.rotation = NAN;
  r.latitude = r.longitude = NAN;
  r.gpsAltitude = r.speed = r.hdop = NAN;
  r.satellites = -1;

  // BMP390
  if (bmpOk && bmp.performReading()) {
    r.temperature = bmp.temperature;
    r.pressure = bmp.pressure / 100.0f;
    r.altitude = bmp.readAltitude(SEA_LEVEL_HPA);

    if (isnan(groundAltitude)) groundAltitude = r.altitude;

    r.altRel = r.altitude - groundAltitude;
  }

  // MPU6050 (solo si está conectado)
  if (mpuOk && !isnan(imu.ax)) {
    r.accelX = imu.ax;
    r.accelY = imu.ay;
    r.accelZ = imu.az;

    r.gForce = sqrtf(
      imu.ax * imu.ax +
      imu.ay * imu.ay +
      imu.az * imu.az
    ) / G;

    r.gMax = imu.gMax;

    // Calcula la inclinación a partir de la gravedad.
    r.pitch = atan2f(
      -imu.ax,
      sqrtf(imu.ay * imu.ay + imu.az * imu.az)
    ) * 180.0f / PI;

    r.roll = atan2f(imu.ay, imu.az) * 180.0f / PI;

    r.rotation = sqrtf(
      imu.gx * imu.gx +
      imu.gy * imu.gy +
      imu.gz * imu.gz
    ) * 180.0f / PI;

    imu.gMax = r.gForce;
  }

  // GPS
  if (gps.location.isValid() && gps.location.age() < 2000) {
    r.latitude = gps.location.lat();
    r.longitude = gps.location.lng();
  }

  if (gps.altitude.isValid() && gps.altitude.age() < 2000) {
    r.gpsAltitude = gps.altitude.meters();
  }

  if (gps.speed.isValid() && gps.speed.age() < 2000) {
    r.speed = gps.speed.kmph();
  }

  if (gps.satellites.isValid()) {
    r.satellites = gps.satellites.value();
  }

  if (gps.hdop.isValid() && gps.hdop.value() > 0) {
    r.hdop = gps.hdop.hdop();
  }

  r.rssi = WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0;
  r.uptime = millis() / 1000;

  return r;
}

// ---------------------------------------------------------------- BUFFER
void pushBuffer(const Reading &r) {
  if (bufCount == BUFFER_SIZE) {
    // Si está lleno, descarta la lectura más antigua.
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

  if (r.tsMs) o["ts"] = r.tsMs;

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

  if (lastTry && millis() - lastTry < 10000) return;

  lastTry = millis();

  Serial.printf("Conectando a WiFi \"%s\"...\n", WIFI_SSID);

  WiFi.disconnect();
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
}

void onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
    Serial.printf(
      "WiFi conectado · IP %s · señal %d dBm\n",
      WiFi.localIP().toString().c_str(),
      WiFi.RSSI()
    );

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

  for (int i = 0; i < n; i++) {
    toJson(arr.add<JsonObject>(), buffer[(bufHead + i) % BUFFER_SIZE]);
  }

  String body;
  serializeJson(doc, body);

  // Conserva el envío HTTPS original al servidor.
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

    Serial.printf(
      "  Envío correcto: %d lectura(s) en %lu ms · pendientes: %d\n",
      n, millis() - t0, bufCount
    );

    return true;
  }

  if (code < 0) {
    // Diagnóstico de conexión con el servidor.
    char err[120] = "";
    tls.lastError(err, sizeof(err));

    IPAddress ip;
    bool dns = WiFi.hostByName("mision-domuyo.up.railway.app", ip);

    Serial.printf(
      "  Error de conexión: %s · TLS: %s · DNS: %s · señal %d dBm\n",
      HTTPClient::errorToString(code).c_str(),
      err,
      dns ? ip.toString().c_str() : "FALLÓ",
      WiFi.RSSI()
    );

    tls.stop();

  } else {
    Serial.printf("  Error HTTP %d: %s\n", code, resp.c_str());
  }

  if (code == 400) {
    // Descarta datos inválidos para evitar que bloqueen la cola.
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

  // Registra el aviso de sincronización NTP antes de configurar la hora.
  sntp_set_time_sync_notification_cb(onNtpSync);

  // Inicializa la tarjeta SD sin impedir que el resto del firmware funcione si falla.
  initSD();

  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(400000);

  // BMP390
  bmpOk = bmp.begin_I2C(0x77) || bmp.begin_I2C(0x76);

  if (bmpOk) {
    bmp.setTemperatureOversampling(BMP3_OVERSAMPLING_2X);
    bmp.setPressureOversampling(BMP3_OVERSAMPLING_8X);
    bmp.setIIRFilterCoeff(BMP3_IIR_FILTER_COEFF_3);
    bmp.setOutputDataRate(BMP3_ODR_50_HZ);
    bmp.performReading();

    Serial.println("BMP390 OK");
  } else {
    Serial.println("BMP390 no encontrado: revisar SDA/SCL y alimentación");
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

  // WiFi + HTTPS
  tls.setInsecure();
  http.setReuse(true);
  http.setTimeout(5000);

  WiFi.mode(WIFI_STA);
  WiFi.setTxPower(WIFI_POWER_8_5dBm);

  delay(200);

  WiFi.setAutoReconnect(true);
  WiFi.onEvent(onWiFiEvent);
  connectWiFi();
}

void loop() {
  // El GPS manda datos todo el tiempo: hay que leerlos siempre.
  while (GPSSerial.available()) {
    gps.encode(GPSSerial.read());
  }

  // Se conserva la sincronización GPS para el reloj interno si todavía no hay NTP.
  syncClockFromGps();

  uint32_t now = millis();

  // Acelerómetro a 50 Hz para registrar los picos de G.
  if (now - lastImu >= IMU_INTERVAL_MS) {
    lastImu = now;
    readImu();
  }

  // Una lectura cada 0,5 s, a ritmo fijo.
  if (now - lastSample >= SAMPLE_INTERVAL_MS) {
    lastSample = (now - lastSample > 2 * SAMPLE_INTERVAL_MS)
      ? now
      : lastSample + SAMPLE_INTERVAL_MS;

    Reading r = sample();
    pushBuffer(r);

    // Guarda localmente la lectura si ya existe una fecha NTP válida.
    saveToSD(r);

    Serial.printf(
      "#%u  T=%.2f°C  P=%.2f hPa  Alt=%.1f m (rel %.1f)",
      r.seq, r.temperature, r.pressure, r.altitude, r.altRel
    );

    if (mpuOk) {
      Serial.printf(
        "  G=%.2f (máx %.2f)  incl=%.0f°/%.0f°",
        r.gForce, r.gMax, r.pitch, r.roll
      );
    }

    // Diagnóstico del GPS.
    if (gps.charsProcessed() < 10) {
      Serial.println("  GPS: NO RECIBE DATOS -> revisar TX del GPS en RX2/D16, VCC y GND");

    } else if (gps.passedChecksum() == 0) {
      Serial.println("  GPS: llegan datos pero son ilegibles -> revisar velocidad o cableado");

    } else if (isnan(r.latitude)) {
      Serial.printf(
        "  GPS: conectado OK, buscando satélites (%d a la vista)\n",
        max(r.satellites, 0)
      );

    } else {
      Serial.printf(
        "  GPS: %.6f, %.6f (%d satélites)\n",
        r.latitude, r.longitude, r.satellites
      );
    }
  }

  // Envío al servidor: conserva los campos JSON originales.
  if (bufCount && now - lastSend >= SEND_INTERVAL_MS) {
    lastSend = now;
    connectWiFi();
    flush();
  }
}