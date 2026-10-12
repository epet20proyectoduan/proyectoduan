/*
 * ============================================================
 *  Titan ATLAS · Misión Domuyo — Firmware de telemetría
 *  ESP32-S + BMP390 + GPS GY-GPS6MV2 (NEO-6M)
 *  + MPU6050 (opcional) + SD + 2 servos MG90S
 *
 *  Frecuencia de telemetría: 2 lecturas por segundo
 *  Activación de servos: altitud relativa > 15 m
 * ============================================================
 *
 *  CONEXIONES (ESP32-S de 30 pines)
 *
 *    BMP390:   VCC → 3V3 · GND → GND · SDA → D21 · SCL → D22
 *    GPS:      VCC → 3V3 · GND → GND · TX → RX2/D16 · RX → TX2/D17
 *    MPU6050:  VCC → 3V3 · GND → GND · SDA → D21 · SCL → D22
 *    SD:       SCK → D18 · MISO/DO → D19 · MOSI/DI → D23 · CS → D27
 *    Servo 1:  señal → GPIO25
 *    Servo 2:  señal → GPIO2
 *
 *  Los servos necesitan alimentación adecuada y GND común
 *  con el ESP32. No alimentar ambos servos desde el pin 3V3.
 *
 *  LIBRERÍAS:
 *    - Adafruit BMP3XX Library
 *    - Adafruit Unified Sensor
 *    - Adafruit BusIO
 *    - Adafruit MPU6050
 *    - TinyGPSPlus
 *    - ArduinoJson v7
 *    - ESP32Servo 3.2.1
 *
 *  También se utiliza esp_sntp, incluida en el core ESP32.
 *
 *  Requiere secrets.h con WIFI_SSID, WIFI_PASSWORD,
 *  SERVER_URL y API_KEY.
 *
 *  SD:
 *    Archivo: /salidas.csv
 *    Fecha UTC: exclusivamente cuando NTP sincronizó.
 *
 *  SERVOS:
 *    Posición inicial: 0 grados.
 *    Disparador: altitud relativa > 15 m.
 *    Posición final: 180 grados.
 *    Activación: una sola vez por ejecución.
 * ============================================================
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
#include <math.h>
#include <time.h>
#include "secrets.h"

// ----------------------------------------------------------------
// CONFIGURACIÓN GENERAL

#define DEVICE_ID          "titan-atlas-01"
#define SAMPLE_INTERVAL_MS 500
#define SEND_INTERVAL_MS   500
#define IMU_INTERVAL_MS    20
#define BUFFER_SIZE        600
#define BATCH_SIZE         40

// ----------------------------------------------------------------
// I2C Y BMP390

#define I2C_SDA       21
#define I2C_SCL       22
#define SEA_LEVEL_HPA 1013.25f

// ----------------------------------------------------------------
// GPS

#define GPS_RX_PIN   16
#define GPS_TX_PIN   17
#define GPS_BAUD     9600
#define GPS_RATE_MS  500

// ----------------------------------------------------------------
// MPU6050

#define ACCEL_RANGE MPU6050_RANGE_8_G
#define GYRO_RANGE  MPU6050_RANGE_500_DEG

const float G = 9.80665f;

// ----------------------------------------------------------------
// TARJETA SD

#define SD_SCK_PIN  18
#define SD_MISO_PIN 19
#define SD_MOSI_PIN 23
#define SD_CS_PIN   27

#define SD_FILE "/salidas.csv"

// ----------------------------------------------------------------
// SERVOS MG90S

#define SERVO1_PIN 25
#define SERVO2_PIN 2

#define SERVO_INITIAL_ANGLE  0
#define SERVO_FINAL_ANGLE    180

#define ALTITUDE_THRESHOLD_M 15.0f

// ----------------------------------------------------------------
// OBJETOS

Adafruit_BMP3XX bmp;
Adafruit_MPU6050 mpu;
TinyGPSPlus gps;
HardwareSerial GPSSerial(2);

WiFiClientSecure tls;
HTTPClient http;

Servo servo1;
Servo servo2;

// ----------------------------------------------------------------
// ESTADO GENERAL

bool sdOk = false;
bool ntpTimeValid = false;
bool dateHeaderWritten = false;

bool bmpOk = false;
bool mpuOk = false;

// Estado de inicialización de cada servo
bool servo1Ok = false;
bool servo2Ok = false;

// Evita que los servos vuelvan a activarse
bool servosTriggered = false;

// Referencia de altitud del firmware original
float groundAltitude = NAN;


// ESTADO DEL MPU6050

struct ImuState {
  float ax, ay, az;
  float gx, gy, gz;
  float gMax;
} imu = {NAN, NAN, NAN, NAN, NAN, NAN, 0};

// ESTRUCTURA DE TELEMETRÍA

struct Reading {
  uint32_t seq;
  uint64_t tsMs;

  float temperature;
  float pressure;
  float altitude;
  float altRel;

  float accelX, accelY, accelZ;
  float gForce, gMax;
  float pitch, roll, rotation;

  double latitude, longitude;

  float gpsAltitude;
  float speed;
  float hdop;

  int satellites;
  int rssi;

  uint32_t uptime;
};

Reading buffer[BUFFER_SIZE];

int bufHead = 0;
int bufCount = 0;

uint32_t seq = 0;
uint32_t lastSample = 0;
uint32_t lastSend = 0;
uint32_t lastImu = 0;

// ----------------------------------------------------------------
// GPS: CONFIGURACIÓN UBX

void sendUbx(const uint8_t *msg, size_t len) {
  uint8_t a = 0;
  uint8_t b = 0;

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
  const uint8_t off[][2] = {
    {0xF0, 0x01},
    {0xF0, 0x02},
    {0xF0, 0x03},
    {0xF0, 0x05}
  };

  for (auto &m : off) {
    uint8_t msg[] = {
      0x06, 0x01, 0x03, 0x00,
      m[0], m[1], 0x00
    };

    sendUbx(msg, sizeof(msg));
    delay(30);
  }

  uint8_t rate[] = {
    0x06, 0x08, 0x06, 0x00,
    (uint8_t)(GPS_RATE_MS & 0xFF),
    (uint8_t)(GPS_RATE_MS >> 8),
    0x01, 0x00,
    0x01, 0x00
  };

  sendUbx(rate, sizeof(rate));
}

// ----------------------------------------------------------------
// HORA Y NTP

uint64_t nowMs() {
  struct timeval tv;
  gettimeofday(&tv, nullptr);

  if (tv.tv_sec < 1700000000) {
    return 0;
  }

  return (uint64_t)tv.tv_sec * 1000ULL
       + tv.tv_usec / 1000;
}

void onNtpSync(struct timeval *tv) {
  (void)tv;

  ntpTimeValid = true;
  Serial.println("Hora sincronizada por NTP");
}

// Sincroniza el reloj interno desde el GPS si todavía no hay NTP.

void syncClockFromGps() {
  static uint32_t lastSync = 0;

  if (ntpTimeValid) return;

  if (nowMs() && millis() - lastSync < 600000) {
    return;
  }

  if (!gps.date.isValid() ||
      !gps.time.isValid() ||
      gps.date.year() < 2024 ||
      gps.time.age() > 500) {
    return;
  }

  struct tm t = {};

  t.tm_year = gps.date.year() - 1900;
  t.tm_mon  = gps.date.month() - 1;
  t.tm_mday = gps.date.day();
  t.tm_hour = gps.time.hour();
  t.tm_min  = gps.time.minute();
  t.tm_sec  = gps.time.second();

  struct timeval tv = {
    mktime(&t),
    (suseconds_t)(gps.time.centisecond() * 10000)
  };

  settimeofday(&tv, nullptr);

  lastSync = millis();
}

bool getNtpDate(char *dateText, size_t dateSize) {
  if (!ntpTimeValid) {
    return false;
  }

  struct timeval tv;
  gettimeofday(&tv, nullptr);

  if (tv.tv_sec < 1700000000) {
    return false;
  }

  time_t seconds = tv.tv_sec;
  struct tm timeInfo = {};

  if (gmtime_r(&seconds, &timeInfo) == nullptr) {
    return false;
  }

  strftime(dateText, dateSize, "%Y-%m-%d", &timeInfo);

  return true;
}

// ----------------------------------------------------------------
// TARJETA SD: FUNCIONES DE ESCRITURA

void printNamedFloat(
  File &file,
  const char *name,
  float value,
  uint8_t decimals = 2
) {
  file.print(name);
  file.print(':');

  if (isfinite(value)) {
    file.print(value, decimals);
  }
}

void printNamedDouble(
  File &file,
  const char *name,
  double value,
  uint8_t decimals = 6
) {
  file.print(name);
  file.print(':');

  if (isfinite(value)) {
    file.print(value, decimals);
  }
}

void initSD() {
  SPI.begin(
    SD_SCK_PIN,
    SD_MISO_PIN,
    SD_MOSI_PIN,
    SD_CS_PIN
  );

  if (!SD.begin(SD_CS_PIN, SPI)) {
    Serial.println("Error: no se pudo inicializar la SD");
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

  if (file.size() == 0) {
    file.println("# Titan ATLAS - Mision Domuyo");
    file.flush();
  }

  file.close();
}

void saveToSD(const Reading &r) {
  if (!sdOk) return;

  char dateText[11];

  // La SD solo registra datos cuando NTP es válido.
  if (!getNtpDate(dateText, sizeof(dateText))) {
    return;
  }

  File file = SD.open(SD_FILE, FILE_APPEND);

  if (!file) {
    Serial.println("Error: no se pudo abrir salidas.csv");
    return;
  }

  if (!dateHeaderWritten) {
    file.print("# Fecha: ");
    file.println(dateText);
    file.flush();

    dateHeaderWritten = true;
  }

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

// SERVOS: INICIALIZACIÓN

void initServos() {
  // Frecuencia estándar de control de servos.
  servo1.setPeriodHertz(50);
  servo2.setPeriodHertz(50);

  // attach() devuelve el canal asignado; -1 indica error.
  int servo1Channel = servo1.attach(SERVO1_PIN, 1000, 2000);
  int servo2Channel = servo2.attach(SERVO2_PIN, 1000, 2000);

  servo1Ok = (servo1Channel >= 0);
  servo2Ok = (servo2Channel >= 0);

  if (servo1Ok) {
    servo1.write(SERVO_INITIAL_ANGLE);
    Serial.println("Servo 1 inicializado en 0 grados");
  } else {
    Serial.println("Error al inicializar servo 1 (GPIO25)");
  }

  if (servo2Ok) {
    servo2.write(SERVO_INITIAL_ANGLE);
    Serial.println("Servo 2 inicializado en 0 grados");
  } else {
    Serial.println("Error al inicializar servo 2 (GPIO2)");
  }

  if (!servo1Ok || !servo2Ok) {
    Serial.println(
      "Advertencia: se mantiene la telemetria aunque falle un servo"
    );
  }

  servosTriggered = false;
}

// SERVOS: ACTIVACIÓN POR ALTITUD RELATIVA

void checkServoTrigger(float relativeAltitude) {
  // No activar hasta que la altitud sea válida.
  if (!isfinite(relativeAltitude)) {
    return;
  }

  // La condición solo puede dispararse una vez.
  if (servosTriggered) {
    return;
  }

  if (relativeAltitude > ALTITUDE_THRESHOLD_M) {
    servosTriggered = true;

    Serial.println();
    Serial.println("==================================");
    Serial.println("UMBRAL DE ALTITUD SUPERADO");
    Serial.print("Altitud relativa: ");
    Serial.print(relativeAltitude, 2);
    Serial.println(" m");
    Serial.println("Activando servos...");
    Serial.println("==================================");

    if (servo1Ok) {
      servo1.write(SERVO_FINAL_ANGLE);
      Serial.println("Servo 1: posicion final 180 grados");
    }

    if (servo2Ok) {
      servo2.write(SERVO_FINAL_ANGLE);
      Serial.println("Servo 2: posicion final 180 grados");
    }

    Serial.println();
  }
}

// SENSORES: MPU6050

void readImu() {
  if (!mpuOk) return;

  sensors_event_t a, g, t;

  if (!mpu.getEvent(&a, &g, &t)) {
    return;
  }

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

  if (gNow > imu.gMax) {
    imu.gMax = gNow;
  }
}

// SENSORES: CAPTURA DE TELEMETRÍA

Reading sample() {
  Reading r;

  r.seq = seq++;
  r.tsMs = nowMs();
  r.temperature = NAN;
  r.pressure = NAN;
  r.altitude = NAN;
  r.altRel = NAN;
  r.accelX = NAN;
  r.accelY = NAN;
  r.accelZ = NAN;
  r.gForce = NAN;
  r.gMax = NAN;
  r.pitch = NAN;
  r.roll = NAN;
  r.rotation = NAN;
  r.latitude = NAN;
  r.longitude = NAN;
  r.gpsAltitude = NAN;
  r.speed = NAN;
  r.hdop = NAN;

  r.satellites = -1;

  // BMP390
  //
  // Se conserva el cálculo de altitud relativa del firmware original:
  //
  // groundAltitude = primera altitud válida de la ejecución
  // altRel = altitud actual - groundAltitude
  //
  // No se reemplaza por el cálculo barométrico de la simulación.

  if (bmpOk && bmp.performReading()) {
    r.temperature = bmp.temperature;
    r.pressure = bmp.pressure / 100.0f;
    r.altitude = bmp.readAltitude(SEA_LEVEL_HPA);

    if (isfinite(r.altitude)) {
      if (isnan(groundAltitude)) {
        groundAltitude = r.altitude;

        Serial.print("Referencia inicial de altitud: ");
        Serial.print(groundAltitude, 2);
        Serial.println(" m");
      }

      r.altRel = r.altitude - groundAltitude;
    }
  }

  // MPU6050

  if (mpuOk && isfinite(imu.ax)) {
    r.accelX = imu.ax;
    r.accelY = imu.ay;
    r.accelZ = imu.az;

    r.gForce = sqrtf(
      imu.ax * imu.ax +
      imu.ay * imu.ay +
      imu.az * imu.az
    ) / G;

    r.gMax = imu.gMax;

    r.pitch = atan2f(
      -imu.ax,
      sqrtf(imu.ay * imu.ay + imu.az * imu.az)
    ) * 180.0f / PI;

    r.roll = atan2f(
      imu.ay,
      imu.az
    ) * 180.0f / PI;

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

  r.rssi = (
    WiFi.status() == WL_CONNECTED
  ) ? WiFi.RSSI() : 0;

  r.uptime = millis() / 1000;

  return r;
}


// BUFFER DE TELEMETRÍA

void pushBuffer(const Reading &r) {
  if (bufCount == BUFFER_SIZE) {
    bufHead = (bufHead + 1) % BUFFER_SIZE;
    bufCount--;
  }

  buffer[(bufHead + bufCount) % BUFFER_SIZE] = r;
  bufCount++;
}

void addIf(JsonObject o, const char *k, double v) {
  if (isfinite(v)) {
    o[k] = v;
  }
}

void toJson(JsonObject o, const Reading &r) {
  o["device_id"] = DEVICE_ID;
  o["seq"] = r.seq;

  if (r.tsMs) {
    o["ts"] = r.tsMs;
  }

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

  if (r.satellites >= 0) {
    o["satellites"] = r.satellites;
  }

  if (r.rssi) {
    o["rssi"] = r.rssi;
  }

  o["uptime_s"] = r.uptime;
}

// WIFI

void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) {
    return;
  }

  static uint32_t lastTry = 0;

  if (lastTry && millis() - lastTry < 10000) {
    return;
  }

  lastTry = millis();

  Serial.printf(
    "Conectando a WiFi \"%s\"...\n",
    WIFI_SSID
  );

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

    configTime(
      0,
      0,
      "pool.ntp.org",
      "time.google.com"
    );

  } else if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
    uint8_t r = info.wifi_sta_disconnected.reason;

    const char *why =
      r == 201 ? "no se encuentra la red" :(r == 202 || r == 15 || r == 204)? "contraseña incorrecta" :
      r == 203 ? "la red rechazó la conexión" :
      r == 200 ? "señal muy débil" :(r == 8 || r == 36)? "reintentando conexión" :
      "otro motivo";

    Serial.printf(
      "WiFi desconectado · motivo %u: %s\n",
      r,
      why
    );
  }
}

// ----------------------------------------------------------------
// ENVÍO HTTPS

bool flush() {
  if (!bufCount || WiFi.status() != WL_CONNECTED) {
    return false;
  }

  int n = min(bufCount, BATCH_SIZE);

  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();

  for (int i = 0; i < n; i++) {
    toJson(
      arr.add<JsonObject>(),
      buffer[(bufHead + i) % BUFFER_SIZE]
    );
  }

  String body;
  serializeJson(doc, body);

  if (!http.begin(tls, SERVER_URL)) {
    return false;
  }

  http.addHeader(
    "Content-Type",
    "application/json"
  );

  http.addHeader("X-API-Key", API_KEY);

  uint32_t t0 = millis();

  int code = http.POST(body);

  String resp = (
    code > 0
  ) ? http.getString() : "";

  http.end();

  if (code == 201) {
    bufHead = (bufHead + n) % BUFFER_SIZE;
    bufCount -= n;

    Serial.printf(
      "  Envío correcto: %d lectura(s) en %lu ms · pendientes: %d\n",
      n,
      millis() - t0,
      bufCount
    );

    return true;
  }

  if (code < 0) {
    char err[120] = "";

    tls.lastError(err, sizeof(err));

    IPAddress ip;

    bool dns = WiFi.hostByName(
      "mision-domuyo.up.railway.app",
      ip
    );

    Serial.printf(
      "  Error de conexión: %s · TLS: %s · DNS: %s · señal %d dBm\n",
      HTTPClient::errorToString(code).c_str(),
      err,
      dns ? ip.toString().c_str() : "FALLÓ",
      WiFi.RSSI()
    );

    tls.stop();

  } else {
    Serial.printf(
      "  Error HTTP %d: %s\n",
      code,
      resp.c_str()
    );
  }

  if (code == 400) {
    bufHead = (bufHead + n) % BUFFER_SIZE;
    bufCount -= n;
  }

  return false;
}

// SETUP

void setup() {
  Serial.begin(115200);
  delay(300);

  Serial.println();
  Serial.println("==================================");
  Serial.println("TITAN ATLAS - MISION DOMUYO");
  Serial.println("Firmware de telemetria + SD + servos");
  Serial.println("Activación automática del paracaidas a mas de 15 m");
  Serial.println("==================================");

  setenv("TZ", "UTC0", 1);
  tzset();

  // Aviso de sincronización NTP.
  sntp_set_time_sync_notification_cb(onNtpSync);

  // Inicializar tarjeta SD.
  initSD();

  // Bus I2C compartido por BMP390 y MPU6050.
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
    Serial.println(
      "BMP390 no encontrado: revisar SDA/SCL y alimentación"
    );

    Serial.println(
      "Los servos no podrán activarse sin altitud válida"
    );
  }
  // MPU6050 (OPCIONAL)

  mpuOk = mpu.begin(0x68) || mpu.begin(0x69);

  if (mpuOk) {
    mpu.setAccelerometerRange(ACCEL_RANGE);
    mpu.setGyroRange(GYRO_RANGE);
    mpu.setFilterBandwidth(MPU6050_BAND_21_HZ);

    Serial.println("MPU6050 OK");

  } else {
    Serial.println(
      "Acelerómetro no conectado: se omite"
    );
  }

  // SERVOS
  initServos();

  // GPS
  GPSSerial.begin(
    GPS_BAUD,
    SERIAL_8N1,
    GPS_RX_PIN,
    GPS_TX_PIN
  );

  delay(100);

  configureGps();

  Serial.println(
    "GPS iniciado a 2 Hz; el primer fix puede tardar varios minutos"
  );

  // WIFI + HTTPS

  tls.setInsecure();
  http.setReuse(true);
  http.setTimeout(5000);
  WiFi.mode(WIFI_STA);
  WiFi.setTxPower(WIFI_POWER_8_5dBm);
  delay(200);
  WiFi.setAutoReconnect(true);
  WiFi.onEvent(onWiFiEvent);

  connectWiFi();

  Serial.println();
  Serial.println("Sistema iniciado");
  Serial.println("Telemetria: 2 lecturas por segundo");
  Serial.println("Umbral de servos: altitud relativa > 15 m");
  Serial.println("Los servos permanecen en 180 grados tras activarse");
}

// ----------------------------------------------------------------
// LOOP PRINCIPAL

void loop() {

  // Leer continuamente el GPS.

  while (GPSSerial.available()) {
    gps.encode(GPSSerial.read());
  }

  // Sincronizar el reloj desde GPS solo si aún no hay NTP.
  syncClockFromGps();

  uint32_t now = millis();
  // Leer MPU6050 a 50 Hz.

  if (now - lastImu >= IMU_INTERVAL_MS) {
    lastImu = now;
    readImu();
  }
  // Captura de telemetría cada 0,5 segundos.

  if (now - lastSample >= SAMPLE_INTERVAL_MS) {
    lastSample = (
      now - lastSample > 2 * SAMPLE_INTERVAL_MS
    ) ? now : lastSample + SAMPLE_INTERVAL_MS;

    Reading r = sample();

    // Evaluar el disparador con la altitud relativa original.
    checkServoTrigger(r.altRel);
    pushBuffer(r);

    // Guardado local condicionado a la sincronización NTP.
    saveToSD(r);
    Serial.printf(
      "#%u  T=%.2f°C  P=%.2f hPa  Alt=%.1f m (rel %.1f)",
      r.seq, r.temperature, r.pressure, r.altitude,r.altRel
    );

    if (mpuOk) {
      Serial.printf(
        "  G=%.2f (máx %.2f)  incl=%.0f°/%.0f°",
        r.gForce,
        r.gMax,
        r.pitch,
        r.roll
      );
    }

    // Diagnóstico GPS.

    if (gps.charsProcessed() < 10) {
      Serial.println(
        "  GPS: no recibe datos; revisar TX del GPS en RX2/D16"
      );

    } else if (gps.passedChecksum() == 0) {
      Serial.println(
        "  GPS: datos ilegibles; revisar velocidad o cableado"
      );

    } else if (isnan(r.latitude)) {
      Serial.printf(
        "  GPS: buscando satélites (%d a la vista)\n",
        max(r.satellites, 0)
      );

    } else {
      Serial.printf(
        "  GPS: %.6f, %.6f (%d satélites)\n",
        r.latitude,
        r.longitude,
        r.satellites
      );
    }
  }

  // --------------------------------------------------------------
  // Envío HTTPS sin alterar los datos de telemetría.

  if (bufCount && now - lastSend >= SEND_INTERVAL_MS) {
    lastSend = now;

    connectWiFi();
    flush();
  }
}