/*
 * ============================================================
 *  Titan ATLAS · Misión Domuyo — Firmware de telemetría
 *  ESP32-S + BMP390 + GPS GY-GPS6MV2 (NEO-6M) + velocímetro
 * ============================================================
 *
 *  CONEXIONES
 *  ----------
 *  BMP390 (I2C)              GPS GY-GPS6MV2 (UART2)       Velocímetro (pulsos)
 *    VIN → 3V3                 VCC → 3V3 (o 5V)             señal → GPIO 27
 *    GND → GND                 GND → GND                    GND   → GND
 *    SDA → GPIO 21             TX  → GPIO 16 (RX2)          VCC   → 3V3
 *    SCL → GPIO 22             RX  → GPIO 17 (TX2)
 *
 *  LIBRERÍAS (Arduino IDE → Herramientas → Administrar bibliotecas)
 *    - Adafruit BMP3XX Library  (instala también Adafruit Unified Sensor y BusIO)
 *    - TinyGPSPlus              (Mikal Hart)
 *    - ArduinoJson              (Benoit Blanchon) v7
 *
 *  PLACA: "ESP32 Dev Module"
 *
 *  ANTES DE COMPILAR: copiar secrets.example.h como secrets.h y completarlo
 *  (red WiFi, URL del servidor y API key). secrets.h no se sube a GitHub.
 *
 *  DATOS QUE ENVÍA (JSON):
 *    temperature, pressure, altitude  → BMP390
 *    alt_rel                          → altura relativa al encendido (BMP390)
 *    latitude, longitude              → GPS
 *    gps_altitude, gps_speed          → GPS
 *    satellites, hdop                 → calidad de la señal GPS
 *    speed                            → velocímetro (km/h)
 *    rssi, uptime_s                   → estado del ESP32
 *
 *  Si se corta el WiFi, guarda las lecturas y las envía por lotes al volver.
 */

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BMP3XX.h>
#include <TinyGPSPlus.h>
#include <ArduinoJson.h>
#include <time.h>
#include "secrets.h"

// ---------------------------------------------------------------- CONFIGURACIÓN
#define DEVICE_ID         "titan-atlas-01"
#define SAMPLE_INTERVAL_MS 2000   // cada cuánto se toma una lectura
#define BUFFER_SIZE        150    // lecturas guardadas sin conexión
#define BATCH_SIZE         30     // lecturas por envío al reconectar

// BMP390
#define I2C_SDA        21
#define I2C_SCL        22
#define SEA_LEVEL_HPA  1013.25    // presión a nivel del mar (ajustar con el dato del día para más precisión)

// GPS GY-GPS6MV2 (NEO-6M)
#define GPS_RX_PIN     16         // al TX del GPS
#define GPS_TX_PIN     17         // al RX del GPS
#define GPS_BAUD       9600

// Velocímetro por pulsos (sensor Hall / reed / encoder)
#define SPEED_PIN           27
#define PULSES_PER_REV      1       // pulsos por vuelta (cantidad de imanes)
#define WHEEL_CIRCUMFERENCE 2.10    // metros recorridos por vuelta
#define SPEED_DEBOUNCE_US   3000    // filtra rebotes (máx. ~333 pulsos/s)

// ---------------------------------------------------------------- OBJETOS
Adafruit_BMP3XX bmp;
TinyGPSPlus gps;
HardwareSerial GPSSerial(2);
WiFiClientSecure tls;

bool bmpOk = false;
float groundAltitude = NAN;   // altura al encender, para calcular alt_rel

// Pulsos del velocímetro (se cuentan en una interrupción)
volatile uint32_t speedPulses = 0;
volatile uint32_t lastPulseUs = 0;
uint32_t lastSpeedCalcMs = 0;

void IRAM_ATTR onSpeedPulse() {
  uint32_t now = micros();
  if (now - lastPulseUs >= SPEED_DEBOUNCE_US) {
    speedPulses++;
    lastPulseUs = now;
  }
}

// ---------------------------------------------------------------- LECTURAS
struct Reading {
  uint32_t seq;
  time_t   ts;
  float temperature, pressure, altitude, altRel;
  double latitude, longitude;
  float gpsAltitude, gpsSpeed, hdop;
  int   satellites;
  float speed;
  int   rssi;
  uint32_t uptime;
};

Reading buffer[BUFFER_SIZE];
int bufHead = 0, bufCount = 0;
uint32_t seq = 0;
uint32_t lastSample = 0;

float readSpeedKmh() {
  noInterrupts();
  uint32_t pulses = speedPulses;
  speedPulses = 0;
  interrupts();
  uint32_t now = millis();
  float seconds = (now - lastSpeedCalcMs) / 1000.0f;
  lastSpeedCalcMs = now;
  if (seconds <= 0) return 0;
  float revs = (float)pulses / PULSES_PER_REV;
  return (revs * WHEEL_CIRCUMFERENCE / seconds) * 3.6f;  // m/s → km/h
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
    setenv("TZ", "UTC0", 1);
    tzset();
    return mktime(&t);
  }
  time_t now = time(nullptr);
  return now > 1700000000 ? now : 0;
}

Reading sample() {
  Reading r;
  r.seq = seq++;
  r.ts = currentTime();
  r.temperature = r.pressure = r.altitude = r.altRel = NAN;
  r.latitude = r.longitude = NAN;
  r.gpsAltitude = r.gpsSpeed = r.hdop = NAN;
  r.satellites = -1;

  if (bmpOk && bmp.performReading()) {
    r.temperature = bmp.temperature;
    r.pressure    = bmp.pressure / 100.0f;  // Pa → hPa
    r.altitude    = bmp.readAltitude(SEA_LEVEL_HPA);
    if (isnan(groundAltitude)) groundAltitude = r.altitude;
    r.altRel = r.altitude - groundAltitude;
  }

  if (gps.location.isValid() && gps.location.age() < 5000) {
    r.latitude  = gps.location.lat();
    r.longitude = gps.location.lng();
  }
  if (gps.altitude.isValid() && gps.altitude.age() < 5000) r.gpsAltitude = gps.altitude.meters();
  if (gps.speed.isValid() && gps.speed.age() < 5000)       r.gpsSpeed = gps.speed.kmph();
  if (gps.satellites.isValid())                            r.satellites = gps.satellites.value();
  if (gps.hdop.isValid() && gps.hdop.value() > 0)          r.hdop = gps.hdop.hdop();

  r.speed  = readSpeedKmh();
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
  addIf(o, "latitude", r.latitude);
  addIf(o, "longitude", r.longitude);
  addIf(o, "gps_altitude", r.gpsAltitude);
  addIf(o, "gps_speed", r.gpsSpeed);
  addIf(o, "hdop", r.hdop);
  if (r.satellites >= 0) o["satellites"] = r.satellites;
  addIf(o, "speed", r.speed);
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

  // BMP390
  Wire.begin(I2C_SDA, I2C_SCL);
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

  // GPS
  GPSSerial.begin(GPS_BAUD, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
  Serial.println("GPS iniciado (el primer fix puede tardar 1-5 min a cielo abierto)");

  // Velocímetro
  pinMode(SPEED_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(SPEED_PIN), onSpeedPulse, FALLING);
  lastSpeedCalcMs = millis();

  // WiFi + HTTPS
  tls.setInsecure();           // para validar el certificado: tls.setCACert(ROOT_CA)
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.onEvent(onWiFiEvent);
  connectWiFi();
}

void loop() {
  // El GPS manda datos todo el tiempo: hay que leerlos siempre
  while (GPSSerial.available()) gps.encode(GPSSerial.read());

  if (millis() - lastSample >= SAMPLE_INTERVAL_MS) {
    lastSample = millis();
    Reading r = sample();
    pushBuffer(r);

    Serial.printf("#%u  T=%.2f°C  P=%.2f hPa  Alt=%.1f m (rel %.1f)  GPS=%s sats=%d  v=%.1f km/h\n",
                  r.seq, r.temperature, r.pressure, r.altitude, r.altRel,
                  isnan(r.latitude) ? "sin fix" : "OK", r.satellites, r.speed);

    connectWiFi();
    while (bufCount && flush()) {
      while (GPSSerial.available()) gps.encode(GPSSerial.read());
    }
  }
}
