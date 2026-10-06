/*
 * ============================================================
 *  Titan ATLAS · Misión Domuyo — Firmware de telemetría
 *  ESP32-S + BMP390 + GPS GY-GPS6MV2 (NEO-6M) + acelerómetro MPU6050
 * ============================================================
 *
 *  CONEXIONES
 *  ----------
 *  El BMP390 y el MPU6050 comparten el bus I2C (direcciones distintas: 0x77 y 0x68).
 *
 *  BMP390 (I2C)        MPU6050 / GY-521 (I2C)     GPS GY-GPS6MV2 (UART2)
 *    VIN → 3V3           VCC → 3V3                  VCC → 3V3 (o 5V)
 *    GND → GND           GND → GND                  GND → GND
 *    SDA → GPIO 21       SDA → GPIO 21              TX  → GPIO 16 (RX2)
 *    SCL → GPIO 22       SCL → GPIO 22              RX  → GPIO 17 (TX2)
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
 *  DATOS QUE ENVÍA (JSON):
 *    temperature, pressure, altitude   → BMP390
 *    alt_rel                           → altura relativa al encendido (BMP390)
 *    accel_x, accel_y, accel_z         → aceleración por eje (m/s²)
 *    g_force                           → aceleración total (en g)
 *    g_max                             → pico de aceleración desde la lectura anterior (g)
 *    pitch, roll                       → inclinación (grados)
 *    rotation                          → velocidad de giro (°/s)
 *    latitude, longitude, gps_altitude → GPS
 *    speed                             → velocidad según el GPS (km/h)
 *    satellites, hdop                  → calidad de la señal GPS
 *    rssi, uptime_s                    → estado del ESP32
 *
 *  Si se corta el WiFi, guarda las lecturas y las envía por lotes al volver.
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
#include <time.h>
#include "secrets.h"

// ---------------------------------------------------------------- CONFIGURACIÓN
#define DEVICE_ID          "titan-atlas-01"
#define SAMPLE_INTERVAL_MS 2000   // cada cuánto se arma y envía una lectura
#define IMU_INTERVAL_MS    20     // el acelerómetro se lee a 50 Hz para no perder picos
#define BUFFER_SIZE        150    // lecturas guardadas sin conexión
#define BATCH_SIZE         30     // lecturas por envío al reconectar

// I2C (BMP390 + MPU6050)
#define I2C_SDA        21
#define I2C_SCL        22
#define SEA_LEVEL_HPA  1013.25    // presión a nivel del mar (ajustar con el dato del día para más precisión)

// GPS GY-GPS6MV2 (NEO-6M)
#define GPS_RX_PIN     16         // al TX del GPS
#define GPS_TX_PIN     17         // al RX del GPS
#define GPS_BAUD       9600

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

bool bmpOk = false;
bool mpuOk = false;
float groundAltitude = NAN;   // altura al encender, para calcular alt_rel

// Estado del acelerómetro (se actualiza a 50 Hz)
struct ImuState {
  float ax, ay, az;     // m/s²
  float gx, gy, gz;     // rad/s
  float gMax;           // pico de g desde la última lectura enviada
} imu = {NAN, NAN, NAN, NAN, NAN, NAN, 0};
uint32_t lastImu = 0;

// ---------------------------------------------------------------- LECTURAS
struct Reading {
  uint32_t seq;
  time_t   ts;
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
uint32_t lastSample = 0;

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

// Hora: primero la del GPS; si no hay, la de internet (NTP); si no, 0
time_t currentTime() {
  if (gps.date.isValid() && gps.time.isValid() && gps.date.year() >= 2024 && gps.time.age() < 3000) {
    struct tm t = {};
    t.tm_year = gps.date.year() - 1900;
    t.tm_mon  = gps.date.month() - 1;
    t.tm_mday = gps.date.day();
    t.tm_hour = gps.time.hour();
    t.tm_min  = gps.time.minute();
    t.tm_sec  = gps.time.second();
    return mktime(&t);   // TZ = UTC (configurado en setup)
  }
  time_t now = time(nullptr);
  return now > 1700000000 ? now : 0;
}

Reading sample() {
  Reading r;
  r.seq = seq++;
  r.ts = currentTime();
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

  // MPU6050
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
  if (gps.location.isValid() && gps.location.age() < 5000) {
    r.latitude  = gps.location.lat();
    r.longitude = gps.location.lng();
  }
  if (gps.altitude.isValid() && gps.altitude.age() < 5000) r.gpsAltitude = gps.altitude.meters();
  if (gps.speed.isValid() && gps.speed.age() < 5000)       r.speed = gps.speed.kmph();
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
  if (r.ts) o["ts"] = (uint32_t)r.ts;
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

void onWiFiEvent(WiFiEvent_t event) {
  if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
    Serial.printf("WiFi conectado · IP %s · señal %d dBm\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());
    configTime(0, 0, "pool.ntp.org", "time.google.com");
  } else if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
    Serial.println("WiFi desconectado");
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

  HTTPClient http;
  http.setTimeout(10000);
  if (!http.begin(tls, SERVER_URL)) return false;
  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-API-Key", API_KEY);
  int code = http.POST(body);
  String resp = http.getString();
  http.end();

  if (code == 201) {
    bufHead = (bufHead + n) % BUFFER_SIZE;
    bufCount -= n;
    Serial.printf("✔ %d lectura(s) enviada(s) · pendientes: %d\n", n, bufCount);
    return true;
  }
  Serial.printf("✘ HTTP %d: %s\n", code, resp.c_str());
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
  setenv("TZ", "UTC0", 1);
  tzset();

  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(400000);

  // BMP390
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

  // MPU6050
  mpuOk = mpu.begin(0x68) || mpu.begin(0x69);
  if (mpuOk) {
    mpu.setAccelerometerRange(ACCEL_RANGE);
    mpu.setGyroRange(GYRO_RANGE);
    mpu.setFilterBandwidth(MPU6050_BAND_21_HZ);
    Serial.println("MPU6050 OK");
  } else {
    Serial.println("⚠ MPU6050 no encontrado: revisar SDA/SCL y alimentación");
  }

  // GPS
  GPSSerial.begin(GPS_BAUD, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
  Serial.println("GPS iniciado (el primer fix puede tardar 1-5 min a cielo abierto)");

  // WiFi + HTTPS
  tls.setInsecure();           // para validar el certificado: tls.setCACert(ROOT_CA)
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

  // Acelerómetro a 50 Hz para registrar los picos de G
  if (millis() - lastImu >= IMU_INTERVAL_MS) {
    lastImu = millis();
    readImu();
  }

  if (millis() - lastSample >= SAMPLE_INTERVAL_MS) {
    lastSample = millis();
    Reading r = sample();
    pushBuffer(r);

    Serial.printf("#%u  T=%.2f°C  P=%.2f hPa  Alt=%.1f m (rel %.1f)", r.seq, r.temperature, r.pressure, r.altitude, r.altRel);
    if (mpuOk) Serial.printf("  G=%.2f (máx %.2f)  incl=%.0f°/%.0f°", r.gForce, r.gMax, r.pitch, r.roll);
    if (isnan(r.latitude)) Serial.printf("  GPS=sin señal (sats %d)\n", max(r.satellites, 0));
    else Serial.printf("  GPS=%.6f, %.6f (sats %d)\n", r.latitude, r.longitude, r.satellites);

    connectWiFi();
    while (bufCount && flush()) {
      while (GPSSerial.available()) gps.encode(GPSSerial.read());
    }
  }
}
