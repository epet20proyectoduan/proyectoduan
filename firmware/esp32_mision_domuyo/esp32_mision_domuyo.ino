/*
 * ============================================================
 *  Misión Domuyo · Titan ATLAS — Firmware ESP32
 *  Envía telemetría en JSON por HTTPS al servidor en Railway.
 * ============================================================
 *
 *  Librerías (Arduino IDE → Gestor de librerías):
 *    - ArduinoJson (Benoit Blanchon) v7
 *    - Adafruit BME280 Library   (solo si USE_BME280 = 1)
 *    - TinyGPSPlus (Mikal Hart)  (solo si USE_GPS = 1)
 *
 *  Placa: "ESP32 Dev Module" (core de Espressif v2.x / v3.x)
 *
 *  Si se pierde el WiFi, las lecturas se guardan en un buffer y se
 *  envían por lotes (un arreglo JSON) cuando vuelve la conexión.
 */

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>

// ---------------------------------------------------------------- CONFIGURACIÓN
#define WIFI_SSID      "TU_WIFI"
#define WIFI_PASSWORD  "TU_PASSWORD"
#define SERVER_URL     "https://TU-APP.up.railway.app/api/telemetry"
#define API_KEY        "LA_MISMA_API_KEY_QUE_EN_RAILWAY"
#define DEVICE_ID      "esp32-01"

#define SEND_INTERVAL_MS 5000   // cada cuánto se toma y envía una lectura
#define BUFFER_SIZE      120    // lecturas guardadas sin conexión
#define BATCH_SIZE       30     // lecturas por envío al reconectar

#define USE_BME280 0            // 1 = sensor BME280 por I2C (temp, hum, presión)
#define USE_GPS    0            // 1 = módulo GPS (NEO-6M/7M/8M) por UART2
#define BATTERY_PIN 34          // ADC con divisor resistivo (o -1 si no hay)
#define BATTERY_DIVIDER 2.0     // relación del divisor (100k/100k = 2.0)

// ---------------------------------------------------------------- SENSORES
#if USE_BME280
  #include <Wire.h>
  #include <Adafruit_BME280.h>
  Adafruit_BME280 bme;
  #define SEA_LEVEL_HPA 1013.25
#endif

#if USE_GPS
  #include <TinyGPSPlus.h>
  TinyGPSPlus gps;
  HardwareSerial GPSSerial(2);
  #define GPS_RX 16
  #define GPS_TX 17
#endif

struct Reading {
  uint32_t seq;
  time_t   ts;          // 0 si todavía no hay hora NTP
  float temperature, humidity, pressure, altitude;
  double latitude, longitude;
  float speed, battery;
  int   rssi;
};

Reading buffer[BUFFER_SIZE];
int bufHead = 0, bufCount = 0;
uint32_t seq = 0;
unsigned long lastSample = 0;

// ---------------------------------------------------------------- utilidades
void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  Serial.printf("Conectando a %s", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) {
    delay(500);
    Serial.print('.');
  }
  Serial.println(WiFi.status() == WL_CONNECTED ? " OK" : " sin conexión (se reintentará)");
  if (WiFi.status() == WL_CONNECTED) {
    configTime(0, 0, "pool.ntp.org", "time.google.com");  // hora UTC
  }
}

float readBattery() {
  if (BATTERY_PIN < 0) return NAN;
  uint32_t mv = analogReadMilliVolts(BATTERY_PIN);
  return mv / 1000.0 * BATTERY_DIVIDER;
}

Reading sample() {
  Reading r;
  r.seq = seq++;
  time_t now = time(nullptr);
  r.ts = now > 1700000000 ? now : 0;
  r.temperature = r.humidity = r.pressure = r.altitude = NAN;
  r.latitude = r.longitude = NAN;
  r.speed = NAN;

#if USE_BME280
  r.temperature = bme.readTemperature();
  r.humidity    = bme.readHumidity();
  r.pressure    = bme.readPressure() / 100.0F;
  r.altitude    = bme.readAltitude(SEA_LEVEL_HPA);
#endif

#if USE_GPS
  if (gps.location.isValid() && gps.location.age() < 5000) {
    r.latitude  = gps.location.lat();
    r.longitude = gps.location.lng();
    if (gps.altitude.isValid()) r.altitude = gps.altitude.meters();  // el GPS tiene prioridad
    if (gps.speed.isValid())    r.speed    = gps.speed.kmph();
  }
#endif

  r.battery = readBattery();
  r.rssi = WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0;
  return r;
}

void pushBuffer(const Reading &r) {
  int idx = (bufHead + bufCount) % BUFFER_SIZE;
  if (bufCount == BUFFER_SIZE) {               // lleno: se descarta la más vieja
    bufHead = (bufHead + 1) % BUFFER_SIZE;
    idx = (bufHead + bufCount - 1) % BUFFER_SIZE;
  } else {
    bufCount++;
  }
  buffer[idx] = r;
}

// Agrega solo los valores válidos (NaN → se omite, el servidor guarda NULL)
void addIf(JsonObject o, const char *k, double v) {
  if (!isnan(v)) o[k] = v;
}

void toJson(JsonObject o, const Reading &r) {
  o["device_id"] = DEVICE_ID;
  o["seq"] = r.seq;
  if (r.ts) o["ts"] = (uint32_t)r.ts;
  addIf(o, "temperature", r.temperature);
  addIf(o, "humidity", r.humidity);
  addIf(o, "pressure", r.pressure);
  addIf(o, "altitude", r.altitude);
  addIf(o, "latitude", r.latitude);
  addIf(o, "longitude", r.longitude);
  addIf(o, "speed", r.speed);
  addIf(o, "battery", r.battery);
  if (r.rssi) o["rssi"] = r.rssi;
  // Sensores extra: cualquier clave adicional se guarda en la columna JSONB "extra"
  // o["uv_index"] = leerUV();
  o["uptime_s"] = millis() / 1000;
}

bool flush() {
  if (!bufCount || WiFi.status() != WL_CONNECTED) return false;

  int n = min(bufCount, BATCH_SIZE);
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  for (int i = 0; i < n; i++) toJson(arr.add<JsonObject>(), buffer[(bufHead + i) % BUFFER_SIZE]);

  String body;
  serializeJson(doc, body);

  WiFiClientSecure client;
  client.setInsecure();  // Para validar el certificado, usar client.setCACert(ROOT_CA)
  HTTPClient http;
  http.setTimeout(10000);
  if (!http.begin(client, SERVER_URL)) return false;
  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-API-Key", API_KEY);

  int code = http.POST(body);
  String resp = http.getString();
  http.end();

  if (code == 201) {
    bufHead = (bufHead + n) % BUFFER_SIZE;
    bufCount -= n;
    Serial.printf("✔ %d lectura(s) enviada(s). Pendientes: %d\n", n, bufCount);
    return true;
  }
  Serial.printf("✘ HTTP %d: %s\n", code, resp.c_str());
  if (code == 400) {  // datos inválidos: descartarlos para no bloquear la cola
    bufHead = (bufHead + n) % BUFFER_SIZE;
    bufCount -= n;
  }
  return false;
}

// ---------------------------------------------------------------- ciclo
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n== Misión Domuyo · Titan ATLAS ==");

#if USE_BME280
  if (!bme.begin(0x76) && !bme.begin(0x77)) Serial.println("BME280 no encontrado");
#endif
#if USE_GPS
  GPSSerial.begin(9600, SERIAL_8N1, GPS_RX, GPS_TX);
#endif
  analogReadResolution(12);
  connectWiFi();
}

void loop() {
#if USE_GPS
  while (GPSSerial.available()) gps.encode(GPSSerial.read());
#endif

  if (millis() - lastSample >= SEND_INTERVAL_MS) {
    lastSample = millis();
    pushBuffer(sample());
    connectWiFi();
    // Vaciar el buffer de a lotes mientras haya conexión
    while (bufCount && flush()) delay(50);
  }
}
