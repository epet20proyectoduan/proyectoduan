/*
 * ============================================================
 *  Titan ATLAS · Misión Domuyo — Firmware de vuelo y telemetría  v3.11
 *  ESP32-S + BMP390 + MPU6050 (opcional)   (sin GPS)
 *  + tarjeta SD + 2 servos MG90S (paracaídas) + medición de batería
 *
 *  Telemetría: 2 lecturas por segundo (cada 0,5 s)
 *  Paracaídas: se abre en el APOGEO (cuando el cohete deja de subir y empieza
 *              a caer 0,5 m), una vez superados 5 m. La altura se controla 10 veces
 *              por segundo. Mientras espera en la rampa, la referencia del suelo
 *              sigue los cambios del clima (no se corre aunque pasen varios minutos).
 * ============================================================
 *
 *  ALIMENTACIÓN (importante)
 *    Baterías 2 × 3,7 V EN SERIE (7,4 V; 8,4 V cargadas), con BMS 2S.
 *      7,4 V → módulo MB102 (entrada 6,5–12 V) → salida 5 V → VIN del ESP32 y VCC de la SD
 *      7,4 V → regulador buck/UBEC 5–6 V, 2–3 A → rojo de los 2 servos
 *      GND común: baterías + MB102 + regulador + ESP32 + servos + SD + sensores
 *    En paralelo (3,7 V) el MB102 no llega a dar 5 V: el ESP32 arranca pero el
 *    BMP390 no responde y el WiFi no conecta.
 *    Recomendado: capacitor de 470–1000 µF cerca de los servos y otro en VIN.
 *
 *  CONEXIONES (placa ESP32-S de 30 pines) — cada cosa en su propio pin
 *    BMP390:   VCC → 3V3 · GND → GND · SDA → D21 · SCL → D22
 *    MPU6050:  VCC → 3V3 · GND → GND · SDA → D27 · SCL → D26  (bus I2C propio)
 *    SD HW-125: VCC → 5 V · GND → GND
 *              SCK → D18 · MISO/DO → D19 · MOSI/DI → D23 · CS → D5  (VSPI estándar)
 *    Servo 1:  naranja → D25 · rojo → +5–6 V del regulador · marrón → GND común
 *    Servo 2:  naranja → D13 · rojo → +5–6 V del regulador · marrón → GND común
 *    Batería:  + de la batería (7,4 V) → resistencia 100 kΩ → D35 → resistencia 47 kΩ → GND
 *              (divisor: con 8,4 V llegan 2,7 V al pin; el ESP32 soporta hasta 3,3 V)
 *              Después de conectarlo, poner BATTERY_PIN en 35 (abajo, en CONFIGURACIÓN).
 *
 *    No usar TX0/RX0 (son del USB) ni D0, D2, D12, D15 (los usa el arranque).
 *    D34, D35, D36 y D39 son solo de entrada (D35 se usa para medir la batería).
 *
 *  FUNCIONA SIN INTERNET: el barómetro, el paracaídas, el acelerómetro y la SD corren
 *    en el loop (núcleo 1) y nunca esperan al WiFi. El envío al servidor corre en una
 *    tarea aparte (núcleo 0): si no hay WiFi o no hay internet, el vuelo no se entera.
 *
 *  WIFI: el ESP32 lo trae incorporado, no lleva cables. Se configura en secrets.h.
 *    Sin conexión (preparación, vuelo, aterrizaje): en la SD se graba todo, 2 por segundo.
 *    En la memoria para enviar después se guardan todas las del vuelo y, en la rampa o
 *    después de aterrizar, 1 cada 5 s: entran ~25 minutos de espera y el vuelo no se pisa.
 *    Todo se envía solo cuando vuelve la conexión, y se envían solas cuando vuelve la conexión.
 *  LED azul de la placa (GPIO2), para saber qué pasa sin notebook:
 *    AL ENCENDER: destellos según el motivo del arranque
 *      1 = encendido normal · 2 = BROWNOUT (caída de tensión) · 3 = error · 4 = watchdog
 *      y queda fijo mientras inicia. Si los destellos se repiten cada pocos
 *      segundos, el ESP32 se está reiniciando (casi siempre por alimentación).
 *    DESPUÉS, funcionando:
 *    titila rápido (2 por segundo) → está enviando datos
 *    un destello corto por segundo → no tiene WiFi
 *    encendido fijo                → tiene WiFi pero el servidor no responde
 *  Sin notebook, los cortes de WiFi, los errores de envío y el motivo de cada
 *  reinicio (por ejemplo BROWNOUT) quedan anotados en la SD y se mandan a la página.
 *
 *  LIBRERÍAS (Arduino IDE → Herramientas → Administrar bibliotecas)
 *    - Adafruit BMP3XX Library (instala también Adafruit Unified Sensor y BusIO)
 *    - Adafruit MPU6050
 *    - ArduinoJson (Benoit Blanchon) v7
 *    - ESP32Servo (Kevin Harrington) 3.x
 *
 *  PLACA: "ESP32 Dev Module" · core "esp32" de Espressif Systems v3.x
 *
 *  ANTES DE COMPILAR: completar secrets.h (red WiFi de 2,4 GHz, URL del
 *  servidor y API key). secrets.h no se sube a GitHub.
 *
 *  REGISTRO EN SD (/salidas.csv, tarjeta en FAT32)
 *    - Cada encendido agrega "# --- Nuevo encendido (motivo) ---".
 *    - Una línea por lectura con el formato nombre:valor.
 *    - El despliegue del paracaídas queda registrado con su altura y hora.
 *    - El archivo queda abierto y se graba cada 2 s (y al instante en el despliegue).
 * ============================================================
 */

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BMP3XX.h>
#include <Adafruit_MPU6050.h>
#include <ArduinoJson.h>
#include <ESP32Servo.h>
#include <esp_sntp.h>
#include <esp_system.h>
#include <sys/time.h>
#include <time.h>
#include <math.h>
#include "secrets.h"

// ================================================================ CONFIGURACIÓN

// General
#define DEVICE_ID          "titan-atlas-01"
#define SAMPLE_INTERVAL_MS 500    // una lectura de telemetría cada 0,5 s
#define SEND_INTERVAL_MS   500    // como máximo un envío cada 0,5 s (con todo lo pendiente)
#define IMU_INTERVAL_MS    20     // el acelerómetro se lee a 50 Hz para no perder picos
#define BARO_INTERVAL_MS   100    // la altura se lee 10 veces por segundo (control del paracaídas)
#define BUFFER_SIZE        300    // lecturas guardadas sin conexión (2,5 minutos a 2 por segundo)
                                  // (se reserva en el heap al arrancar; si no alcanza, usa menos)
#define OFFLINE_KEEP_MS    5000   // sin conexión y fuera de vuelo: 1 lectura cada 5 s a la memoria
#define BATCH_SIZE         20     // lecturas máximas por envío (menos memoria: la conexión HTTPS necesita ~45 KB)
#define SERVER_HOST        "mision-domuyo.up.railway.app"   // solo para el diagnóstico de DNS

// I2C: cada sensor en su propio bus
#define BMP_SDA        21         // bus Wire
#define BMP_SCL        22
#define MPU_SDA        27         // bus Wire1
#define MPU_SCL        26
#define SEA_LEVEL_HPA  1013.25f   // presión a nivel del mar (ajustar con el dato del día)

// Acelerómetro MPU6050
#define ACCEL_RANGE    MPU6050_RANGE_8_G      // 2, 4, 8 o 16 G
#define GYRO_RANGE     MPU6050_RANGE_500_DEG  // 250, 500, 1000 o 2000 °/s

// Tarjeta SD (SPI)
#define SD_SCK_PIN     18
#define SD_MISO_PIN    19
#define SD_MOSI_PIN    23
#define SD_CS_PIN      5
#define SD_FILE        "/salidas.csv"
#define SD_FLUSH_MS    2000       // graba lo pendiente en la tarjeta cada 2 s

// Paracaídas (servos MG90S)
#define SERVO1_PIN            25
#define SERVO2_PIN            13
#define SERVO_INITIAL_ANGLE   0
#define SERVO_FINAL_ANGLE     180
#define SERVO_MIN_US          500     // pulso a 0°   (MG90S: 500–2400 µs = recorrido completo de 180°)
#define SERVO_MAX_US          2400    // pulso a 180°
#define SERVO_HZ              100     // refresco de la señal: 100 Hz da más fuerza que 50 Hz en servos analógicos
                                      // (si un servo vibra o calienta mucho, volver a 50)
#define SERVO_KICK_DEG        40      // al abrir: retrocede 40° y vuelve a empujar, para destrabar
#define DEPLOY_AT_APOGEE      true    // true: abre en el apogeo · false: abre al superar ALTITUDE_THRESHOLD_M
#define ALTITUDE_THRESHOLD_M  5.0f    // altura mínima sobre el suelo para habilitar (armar) el paracaídas
#define APOGEE_DROP_M         0.5f    // metros que tiene que bajar desde la máxima para confirmar el apogeo
#define ALT_FILTER            0.35f   // suavizado de la altura (0-1): más bajo = más suave, más lento
#define GROUND_TRACK_RATE     0.005f  // en espera, el suelo sigue los cambios del clima (~20 s de constante)
#define GROUND_TRACK_MAX_M    2.0f    // ...solo si está a menos de 2 m del suelo (en el despegue se congela)

// Batería: divisor 100 kΩ (arriba) / 47 kΩ (abajo) hacia GPIO35, 2 celdas Li-ion en serie
#define BATTERY_PIN           -1      // poner 35 cuando el divisor esté conectado (-1 = no se mide)
                                      // (GPIO35 es ADC1: funciona aunque el WiFi esté encendido)
#define BATTERY_R_TOP         100.0f  // kΩ
#define BATTERY_R_BOTTOM      47.0f   // kΩ
#define BATTERY_CELLS         2
#define BATTERY_LOW_PCT       20      // aviso de batería baja

// LED azul de la placa: cambia de estado con cada envío correcto al servidor
#define STATUS_LED_PIN        2

const float G = 9.80665f;
const time_t MIN_VALID_EPOCH = 1700000000;   // antes de esto el reloj no está en hora

// ================================================================ OBJETOS Y ESTADO

Adafruit_BMP3XX bmp;
Adafruit_MPU6050 mpu;
WiFiClientSecure tls;
HTTPClient http;
Servo servo1, servo2;
File logFile;

bool bmpOk = false, mpuOk = false, sdOk = false;
bool servo1Ok = false, servo2Ok = false;
bool ntpTimeValid = false;
bool dateHeaderWritten = false;

// Estado del barómetro (se actualiza a 10 Hz)
struct BaroState {
  float temperature;    // °C
  float pressure;       // hPa
  float altitude;       // m s.n.m.
  float altFiltered;    // m sobre el suelo, suavizada (para el paracaídas)
  int   fails;          // lecturas fallidas seguidas
} baro = {NAN, NAN, NAN, NAN, 0};

// Altura del suelo: promedio de las primeras lecturas al encender
float groundAltitude = NAN;
float groundSum = 0;
int   groundCount = 0;
const int GROUND_SAMPLES = 10;

// Paracaídas: 0 = en espera, 1 = armado (pasó la altura mínima), 2 = desplegado
enum DeployState { DEPLOY_WAITING = 0, DEPLOY_ARMED = 1, DEPLOY_DONE = 2 };
DeployState deployState = DEPLOY_WAITING;
float maxAltRel = NAN;           // altura máxima alcanzada sobre el suelo
float deployAltRel = NAN;        // altura a la que se abrió
uint32_t deployAtMs = 0;         // momento en que se abrió (millis)

// Estado del acelerómetro (se actualiza a 50 Hz)
struct ImuState {
  float ax, ay, az;     // m/s²
  float gx, gy, gz;     // rad/s
  float gMax;           // pico de g desde la última lectura
} imu = {NAN, NAN, NAN, NAN, NAN, NAN, 0};

struct Reading {
  uint32_t seq;
  uint64_t tsMs;        // hora de la medición en ms (0 si todavía no hay hora)
  float temperature, pressure, altitude, altRel, maxAltRel;
  float accelX, accelY, accelZ, gForce, gMax, pitch, roll, rotation;
  float battery;        // V
  int   batteryPct;     // %
  uint8_t deploy;       // DeployState
  int rssi;
  uint32_t uptime;
};

// Cola circular de lecturas pendientes de envío. Se reserva en la memoria dinámica (heap)
// al arrancar: la memoria estática del ESP32 es chica y 600 lecturas no entran ahí.
Reading *buffer = nullptr;
int bufferCap = 0;
int bufHead = 0, bufCount = 0;
uint32_t seq = 0;
uint32_t lastSample = 0, lastImu = 0, lastBaro = 0;
uint32_t lastBmpTry = 0;           // reintento del BMP390 si no respondió
uint32_t lastSdFlush = 0;
uint32_t sendBackoffUntil = 0;     // después de un error de conexión, espera antes de reintentar
int sdErrors = 0;

// Diagnóstico sin notebook: el evento de WiFi corre en otra tarea, así que solo anota
// acá y el loop lo escribe en la SD (escribir la SD desde dos tareas a la vez no es seguro)
volatile uint8_t  wifiLastReason = 0;
volatile uint32_t wifiDrops = 0;          // cuántas veces se cortó el WiFi desde el encendido
uint32_t wifiDropsLogged = 0;
uint32_t sendErrors = 0;                  // envíos fallidos desde el encendido
uint32_t lastSendOkMs = 0;                // momento del último envío correcto
const char *bootReason = "";              // motivo del último arranque (texto corto)

// La cola de lecturas la comparten el loop (agrega) y la tarea de envío (saca): se protege
// con un mutex. La tarea de envío no escribe la SD: deja el texto acá y lo graba el loop.
SemaphoreHandle_t bufMutex;
Reading *sendBatch = nullptr;             // copia de las lecturas que se están enviando
volatile uint32_t netEvents = 0;
uint32_t netEventsLogged = 0;
char netEventText[160] = "";

// ================================================================ HORA

uint64_t nowMs() {
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  if (tv.tv_sec < MIN_VALID_EPOCH) return 0;
  return (uint64_t)tv.tv_sec * 1000ULL + tv.tv_usec / 1000;
}

void onNtpSync(struct timeval *tv) {
  (void)tv;
  ntpTimeValid = true;
  Serial.println("Hora sincronizada por NTP");
}

// Fecha UTC "AAAA-MM-DD", si el reloj ya fue puesto en hora por NTP
bool getClockDate(char *dateText, size_t dateSize) {
  if (!ntpTimeValid) return false;
  time_t seconds = time(nullptr);
  if (seconds < MIN_VALID_EPOCH) return false;
  struct tm timeInfo = {};
  if (!gmtime_r(&seconds, &timeInfo)) return false;
  strftime(dateText, dateSize, "%Y-%m-%d", &timeInfo);
  return true;
}

// ================================================================ SENSORES

void initBmp() {
  static bool wireStarted = false;
  if (!wireStarted) {
    Wire.begin(BMP_SDA, BMP_SCL);
    Wire.setClock(100000);   // 100 kHz: más tolerante a cables largos o flojos
    wireStarted = true;
  }
  bmpOk = bmp.begin_I2C(0x77, &Wire) || bmp.begin_I2C(0x76, &Wire);
  if (!bmpOk) {
    Serial.println("BMP390 no encontrado en D21/D22: revisar SDA/SCL y alimentación (se reintenta cada 5 s)");
    return;
  }
  // A 50 Hz internos, con filtro suave: lecturas estables a 10 Hz
  bmp.setTemperatureOversampling(BMP3_OVERSAMPLING_2X);
  bmp.setPressureOversampling(BMP3_OVERSAMPLING_8X);
  bmp.setIIRFilterCoeff(BMP3_IIR_FILTER_COEFF_3);
  bmp.setOutputDataRate(BMP3_ODR_50_HZ);
  bmp.performReading();      // la primera lectura suele ser inexacta
  baro.fails = 0;
  Serial.println("BMP390 OK");
}

// Lee el barómetro (10 veces por segundo) y calcula la altura sobre el suelo
void readBaro() {
  if (!bmpOk) return;
  if (!bmp.performReading()) {
    if (++baro.fails >= 10) {             // 1 s sin respuesta: se lo vuelve a buscar
      bmpOk = false;
      Serial.println("BMP390 dejó de responder");
    }
    return;
  }
  baro.fails = 0;
  baro.temperature = bmp.temperature;
  baro.pressure    = bmp.pressure / 100.0f;                                // Pa → hPa
  baro.altitude    = 44330.0f * (1.0f - powf(baro.pressure / SEA_LEVEL_HPA, 0.1903f));
  if (!isfinite(baro.altitude)) return;

  // Altura del suelo: promedio de las primeras 10 lecturas (1 s quieto en la rampa)
  if (isnan(groundAltitude)) {
    groundSum += baro.altitude;
    if (++groundCount >= GROUND_SAMPLES) {
      groundAltitude = groundSum / groundCount;
      Serial.printf("Referencia del suelo: %.2f m s.n.m.\n", groundAltitude);
    }
    return;
  }

  float rel = baro.altitude - groundAltitude;

  // En la rampa, la presión cambia con el clima: mientras espera y está cerca del suelo,
  // la referencia se corrige de a poco. En el despegue sube rápido y deja de corregirse.
  if (deployState == DEPLOY_WAITING && fabsf(rel) < GROUND_TRACK_MAX_M) {
    groundAltitude += GROUND_TRACK_RATE * rel;
    rel = baro.altitude - groundAltitude;
    if (!isnan(maxAltRel) && maxAltRel > GROUND_TRACK_MAX_M) maxAltRel = NAN;
  }

  baro.altFiltered = isnan(baro.altFiltered) ? rel : baro.altFiltered + ALT_FILTER * (rel - baro.altFiltered);
  if (isnan(maxAltRel) || baro.altFiltered > maxAltRel) maxAltRel = baro.altFiltered;
}

void initMpu() {
  Wire1.begin(MPU_SDA, MPU_SCL);
  Wire1.setClock(400000);
  mpuOk = mpu.begin(0x68, &Wire1) || mpu.begin(0x69, &Wire1);
  if (!mpuOk) {
    Serial.println("Acelerómetro no conectado en SDA D27 / SCL D26 (se omite)");
    return;
  }
  mpu.setAccelerometerRange(ACCEL_RANGE);
  mpu.setGyroRange(GYRO_RANGE);
  mpu.setFilterBandwidth(MPU6050_BAND_21_HZ);
  Serial.println("MPU6050 OK");
}

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

// ================================================================ BATERÍA

void initBattery() {
  if (BATTERY_PIN < 0) return;
  analogReadResolution(12);   // la atenuación por defecto (11 dB) mide hasta ~3,1 V
  pinMode(BATTERY_PIN, INPUT);
}

// Tensión de la batería en V (NAN si el divisor no está conectado)
float readBattery() {
  if (BATTERY_PIN < 0) return NAN;
  uint32_t mv = 0;
  for (int i = 0; i < 16; i++) mv += analogReadMilliVolts(BATTERY_PIN);   // promedio: menos ruido
  float pinV = mv / 16.0f / 1000.0f;
  if (pinV < 0.3f) return NAN;                     // sin divisor conectado
  return pinV * (BATTERY_R_TOP + BATTERY_R_BOTTOM) / BATTERY_R_BOTTOM;
}

// Carga aproximada (%) según la tensión por celda de una Li-ion en reposo
int batteryPercent(float volts) {
  if (!isfinite(volts)) return -1;
  const float cell = volts / BATTERY_CELLS;
  const float v[] = {3.30f, 3.50f, 3.60f, 3.70f, 3.80f, 3.90f, 4.00f, 4.10f, 4.20f};
  const int   p[] = {0,     10,    20,    35,    50,    65,    80,    90,    100};
  if (cell <= v[0]) return 0;
  if (cell >= v[8]) return 100;
  for (int i = 1; i < 9; i++) {
    if (cell <= v[i]) return p[i - 1] + (int)((cell - v[i - 1]) / (v[i] - v[i - 1]) * (p[i] - p[i - 1]));
  }
  return 100;
}

// ================================================================ PARACAÍDAS (SERVOS)

void initServos() {
  servo1.setPeriodHertz(SERVO_HZ);
  servo2.setPeriodHertz(SERVO_HZ);
  // Uno por vez: si arrancan juntos, el pico de corriente puede reiniciar el ESP32
  servo1.attach(SERVO1_PIN, SERVO_MIN_US, SERVO_MAX_US);
  servo1Ok = servo1.attached();
  if (servo1Ok) {
    servo1.write(SERVO_INITIAL_ANGLE);
    Serial.println("Servo 1 inicializado en 0 grados");
  } else {
    Serial.println("Error al inicializar servo 1 (GPIO25)");
  }
  delay(500);

  servo2.attach(SERVO2_PIN, SERVO_MIN_US, SERVO_MAX_US);
  servo2Ok = servo2.attached();
  if (servo2Ok) {
    servo2.write(SERVO_INITIAL_ANGLE);
    Serial.println("Servo 2 inicializado en 0 grados");
  } else {
    Serial.println("Error al inicializar servo 2 (GPIO13)");
  }
  if (!servo1Ok || !servo2Ok) Serial.println("Advertencia: la telemetría sigue aunque falle un servo");
  Serial.printf("Paracaídas: %s (se habilita a más de %.0f m)\n",
                DEPLOY_AT_APOGEE ? "se abre en el apogeo" : "se abre al superar la altura mínima",
                ALTITUDE_THRESHOLD_M);
}

void sdLogEvent(const char *text);

// Apertura con más fuerza: los servos se mueven de a uno (cada uno con toda la corriente)
// y después dan 3 "golpes" (retroceden SERVO_KICK_DEG y vuelven a empujar) para vencer la
// fricción de la traba. Corre desde el loop sin frenar nada. -1 = ese servo no cambia.
struct ServoStep { uint16_t atMs; int16_t a1, a2; };
const int16_t KICK = SERVO_FINAL_ANGLE - SERVO_KICK_DEG;
const ServoStep DEPLOY_STEPS[] = {
  {0,    SERVO_FINAL_ANGLE, -1}, {200,  -1, SERVO_FINAL_ANGLE},
  {700,  KICK, KICK},            {900,  SERVO_FINAL_ANGLE, -1}, {1100, -1, SERVO_FINAL_ANGLE},
  {1600, KICK, KICK},            {1800, SERVO_FINAL_ANGLE, -1}, {2000, -1, SERVO_FINAL_ANGLE},
  {2500, KICK, KICK},            {2700, SERVO_FINAL_ANGLE, -1}, {2900, -1, SERVO_FINAL_ANGLE},
};
const int DEPLOY_STEP_COUNT = sizeof(DEPLOY_STEPS) / sizeof(DEPLOY_STEPS[0]);
int deployStep = DEPLOY_STEP_COUNT;     // = terminado
uint32_t deployStepStart = 0;

void updateServoSequence(uint32_t now) {
  while (deployStep < DEPLOY_STEP_COUNT && now - deployStepStart >= DEPLOY_STEPS[deployStep].atMs) {
    const ServoStep &st = DEPLOY_STEPS[deployStep++];
    if (st.a1 >= 0 && servo1Ok) servo1.write(st.a1);
    if (st.a2 >= 0 && servo2Ok) servo2.write(st.a2);
  }
}

void deployParachute(float altRel) {
  deployState = DEPLOY_DONE;
  deployAltRel = altRel;
  deployAtMs = millis();
  deployStepStart = millis();       // arranca la secuencia de apertura (de a uno + golpes)
  deployStep = 0;
  updateServoSequence(deployStepStart);

  Serial.println("\n==================================");
  Serial.printf("PARACAÍDAS DESPLEGADO a %.1f m (máxima %.1f m)\n", altRel, maxAltRel);
  Serial.println("==================================\n");

  char line[96];
  snprintf(line, sizeof(line), "# PARACAIDAS desplegado a %.2f m (maxima %.2f m), uptime %lu s",
           altRel, maxAltRel, (unsigned long)(millis() / 1000));
  sdLogEvent(line);
}

// Se llama 10 veces por segundo, con la altura suavizada sobre el suelo
void updateDeploy() {
  if (deployState == DEPLOY_DONE || !isfinite(baro.altFiltered)) return;
  const float alt = baro.altFiltered;

  if (deployState == DEPLOY_WAITING && alt > ALTITUDE_THRESHOLD_M) {
    deployState = DEPLOY_ARMED;
    Serial.printf("Paracaídas ARMADO: superó %.0f m\n", ALTITUDE_THRESHOLD_M);
    sdLogEvent("# Paracaidas armado");
    if (!DEPLOY_AT_APOGEE) { deployParachute(alt); return; }
  }

  // Apogeo: ya armado y la altura bajó APOGEE_DROP_M desde la máxima
  if (deployState == DEPLOY_ARMED && DEPLOY_AT_APOGEE && maxAltRel - alt >= APOGEE_DROP_M) {
    deployParachute(alt);
  }
}

const char *deployText(uint8_t s) {
  return s == DEPLOY_DONE ? "desplegado" : s == DEPLOY_ARMED ? "armado" : "en espera";
}

// ================================================================ LECTURA DE TELEMETRÍA

Reading sample() {
  Reading r;
  r.seq = seq++;
  r.tsMs = nowMs();
  r.temperature = r.pressure = r.altitude = r.altRel = r.maxAltRel = NAN;
  r.accelX = r.accelY = r.accelZ = r.gForce = r.gMax = r.pitch = r.roll = r.rotation = NAN;

  // Barómetro: últimos valores leídos a 10 Hz
  if (bmpOk) {
    r.temperature = baro.temperature;
    r.pressure    = baro.pressure;
    r.altitude    = baro.altitude;
    if (!isnan(groundAltitude) && isfinite(baro.altitude)) r.altRel = baro.altitude - groundAltitude;
    r.maxAltRel   = maxAltRel;
  }

  // MPU6050 (solo si está conectado)
  if (mpuOk && isfinite(imu.ax)) {
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

  // Batería y paracaídas
  r.battery    = readBattery();
  r.batteryPct = batteryPercent(r.battery);
  r.deploy     = deployState;

  r.rssi   = WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0;
  r.uptime = millis() / 1000;
  return r;
}

// ================================================================ BUFFER Y JSON

// Reserva el buffer; si no hay lugar para BUFFER_SIZE lecturas, prueba con la mitad
void initBuffer() {
  for (int cap = BUFFER_SIZE; cap >= 50; cap /= 2) {
    buffer = (Reading *)malloc(sizeof(Reading) * cap);
    if (buffer) { bufferCap = cap; break; }
  }
  Serial.printf("Buffer sin conexión: %d lecturas (%.1f min a 2 por segundo) · %u bytes por lectura\n",
                bufferCap, bufferCap / 120.0f, (unsigned)sizeof(Reading));
}

void pushBuffer(const Reading &r) {
  if (!bufferCap) return;
  xSemaphoreTake(bufMutex, portMAX_DELAY);
  if (bufCount == bufferCap) {               // lleno: se descarta la más vieja
    bufHead = (bufHead + 1) % bufferCap;
    bufCount--;
  }
  buffer[(bufHead + bufCount) % bufferCap] = r;
  bufCount++;
  xSemaphoreGive(bufMutex);
}

// Qué lecturas van a la memoria para enviar (a la SD van todas):
//   con WiFi, o en vuelo (despegó, armado, o hasta 60 s después de abrir) → todas
//   sin WiFi, en la rampa o ya en el suelo                               → 1 cada 5 s
bool keepInBuffer(uint32_t now) {
  static uint32_t lastKept = 0;
  bool flying = deployState == DEPLOY_ARMED
             || (deployState == DEPLOY_WAITING && isfinite(baro.altFiltered) && baro.altFiltered > 1.0f)
             || (deployState == DEPLOY_DONE && now - deployAtMs < 60000);
  if (flying || WiFi.status() == WL_CONNECTED || now - lastKept >= OFFLINE_KEEP_MS) {
    lastKept = now;
    return true;
  }
  return false;
}

// Saca de la cola las lecturas ya enviadas (hasta la de número lastSeq inclusive)
void dropSent(uint32_t lastSeq) {
  xSemaphoreTake(bufMutex, portMAX_DELAY);
  while (bufCount && (int32_t)(buffer[bufHead].seq - lastSeq) <= 0) {
    bufHead = (bufHead + 1) % bufferCap;
    bufCount--;
  }
  xSemaphoreGive(bufMutex);
}

// La tarea de envío deja el evento para que el loop lo grabe en la SD
void netLogEvent(const char *text) {
  xSemaphoreTake(bufMutex, portMAX_DELAY);
  strlcpy(netEventText, text, sizeof(netEventText));
  netEvents = netEvents + 1;
  xSemaphoreGive(bufMutex);
}

void addIf(JsonObject o, const char *k, double v) {
  if (isfinite(v)) o[k] = v;
}

void toJson(JsonObject o, const Reading &r) {
  o["device_id"] = DEVICE_ID;
  o["seq"] = r.seq;
  if (r.tsMs) o["ts"] = r.tsMs;   // ms: el servidor ubica cada lectura en su momento exacto
  addIf(o, "temperature", r.temperature);
  addIf(o, "pressure", r.pressure);
  addIf(o, "altitude", r.altitude);
  addIf(o, "alt_rel", r.altRel);
  addIf(o, "max_alt_rel", r.maxAltRel);
  addIf(o, "accel_x", r.accelX);
  addIf(o, "accel_y", r.accelY);
  addIf(o, "accel_z", r.accelZ);
  addIf(o, "g_force", r.gForce);
  addIf(o, "g_max", r.gMax);
  addIf(o, "pitch", r.pitch);
  addIf(o, "roll", r.roll);
  addIf(o, "rotation", r.rotation);
  addIf(o, "battery", r.battery);
  if (r.batteryPct >= 0) o["battery_pct"] = r.batteryPct;
  o["parachute"] = deployText(r.deploy);
  o["sd"] = sdOk ? "grabando" : "sin tarjeta";   // estado de la microSD (se ve en el panel)
  if (r.rssi) o["rssi"] = r.rssi;
  o["uptime_s"] = r.uptime;
  // Diagnóstico: se ve en la página aunque no haya notebook conectada
  o["boot"] = bootReason;
  o["wifi_drops"] = (uint32_t)wifiDrops;
  if (sendErrors) o["send_errors"] = sendErrors;
}

// ================================================================ TARJETA SD

const char *resetReasonText() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:  return "encendido normal";
    case ESP_RST_BROWNOUT: return "BROWNOUT: cayó el voltaje (batería floja o pico de los servos)";
    case ESP_RST_PANIC:    return "error del programa";
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:      return "watchdog: el programa se trabó";
    case ESP_RST_EXT:      return "botón EN";
    case ESP_RST_SW:       return "reinicio por software";
    default:               return "otro motivo";
  }
}

void initSD() {
  SPI.begin(SD_SCK_PIN, SD_MISO_PIN, SD_MOSI_PIN, SD_CS_PIN);
  if (!SD.begin(SD_CS_PIN, SPI)) {
    Serial.println("Error: no se pudo inicializar la tarjeta SD (se sigue sin registro en SD)");
    return;
  }
  // El archivo queda abierto: se graba cada 2 s en vez de abrir y cerrar en cada lectura
  logFile = SD.open(SD_FILE, FILE_APPEND);
  if (!logFile) {
    Serial.println("Error: no se pudo abrir salidas.csv");
    return;
  }
  if (logFile.size() == 0) logFile.println("# Titan ATLAS - Mision Domuyo");
  logFile.printf("# --- Nuevo encendido (motivo: %s) ---\n", resetReasonText());
  logFile.flush();
  sdOk = true;
  Serial.println("Tarjeta SD inicializada");
}

// Si la tarjeta falla varias veces seguidas, se deja de usar (el resto sigue funcionando)
void sdCheckWrite(size_t written) {
  if (written > 0) { sdErrors = 0; return; }
  if (++sdErrors >= 5) {
    sdOk = false;
    logFile.close();
    Serial.println("Error: la tarjeta SD dejó de responder (se sigue sin registro en SD)");
  }
}

void sdLogEvent(const char *text) {
  if (!sdOk) return;
  sdCheckWrite(logFile.println(text));
  logFile.flush();   // los eventos se graban al instante
}

// Escribe ", nombre:valor"; deja el valor vacío si no hay medición válida
void printField(const char *name, double value, uint8_t decimals) {
  logFile.printf(", %s:", name);
  if (isfinite(value)) logFile.print(value, decimals);
}

void saveToSD(const Reading &r) {
  if (!sdOk) return;

  char dateText[11];
  if (!dateHeaderWritten && getClockDate(dateText, sizeof(dateText))) {
    logFile.printf("# Fecha UTC: %s (hora por NTP)\n", dateText);
    dateHeaderWritten = true;
  }

  size_t written = logFile.printf("device_id:%s, seq:%u, ts:", DEVICE_ID, (unsigned)r.seq);
  if (r.tsMs) logFile.print(r.tsMs);
  printField("temperature",  r.temperature, 2);
  printField("pressure",     r.pressure,    2);
  printField("altitude",     r.altitude,    2);
  printField("alt_rel",      r.altRel,      2);
  printField("max_alt_rel",  r.maxAltRel,   2);
  printField("accel_x",      r.accelX,      4);
  printField("accel_y",      r.accelY,      4);
  printField("accel_z",      r.accelZ,      4);
  printField("g_max",        r.gMax,        4);
  printField("rotation",     r.rotation,    2);
  printField("battery",      r.battery,     2);
  logFile.printf(", parachute:%s, rssi:%d, uptime_s:%u\n", deployText(r.deploy), r.rssi, (unsigned)r.uptime);
  sdCheckWrite(written);
}

// ================================================================ RED

// El ESP32 se reconecta solo (setAutoReconnect). Acá solo se fuerza un intento
// nuevo si pasaron 30 s sin conexión, para no cortar un intento que está en curso.
void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  static uint32_t lastTry = 0;
  if (lastTry && millis() - lastTry < 30000) return;
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
      (r == 202 || r == 15 || r == 204) ? "contraseña incorrecta o señal débil" :
      r == 203 ? "la red rechazó la conexión" :
      (r == 200 || r == 2 || r == 4) ? "señal débil o alimentación insuficiente" :
      (r == 8 || r == 36) ? "reintentando conexión" : "otro motivo";
    Serial.printf("WiFi desconectado · motivo %u: %s\n", r, why);
    wifiLastReason = r;
    wifiDrops = wifiDrops + 1;
  }
}

// Cantidad de destellos del LED al arrancar, según el motivo del arranque
int bootBlinkCount() {
  switch (esp_reset_reason()) {
    case ESP_RST_BROWNOUT: return 2;
    case ESP_RST_PANIC:    return 3;
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:      return 4;
    default:               return 1;
  }
}

// Motivo corto del arranque, para mandarlo a la página
const char *bootReasonShort() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:  return "encendido";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_PANIC:    return "error";
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:      return "watchdog";
    case ESP_RST_EXT:      return "boton EN";
    case ESP_RST_SW:       return "software";
    default:               return "otro";
  }
}

// LED azul de la placa como indicador de estado (no bloquea):
//   titila con cada envío correcto → está mandando datos
//   un destello corto por segundo   → no tiene WiFi
//   encendido fijo                  → tiene WiFi pero el servidor no responde
void updateStatusLed(uint32_t now) {
  bool sendingOk = lastSendOkMs && now - lastSendOkMs < 3000;
  if (sendingOk) return;                                   // lo maneja flush() en cada envío
  if (WiFi.status() != WL_CONNECTED) {
    digitalWrite(STATUS_LED_PIN, (now % 1000) < 80 ? HIGH : LOW);
  } else {
    digitalWrite(STATUS_LED_PIN, HIGH);
  }
}

void initWiFi() {
  tls.setInsecure();           // para validar el certificado: tls.setCACert(ROOT_CA)
  http.setReuse(true);         // conexión persistente: cada envío tarda ~100-200 ms
  http.setConnectTimeout(3000);   // si el servidor no responde, no traba el loop más de ~3 s
  http.setTimeout(4000);
  WiFi.mode(WIFI_STA);
  // Potencia intermedia: más alcance que 8,5 dBm sin el consumo máximo
  WiFi.setTxPower(WIFI_POWER_11dBm);
  delay(200);
  WiFi.setAutoReconnect(true);
  WiFi.onEvent(onWiFiEvent);
  connectWiFi();
}

// Corre en la tarea de envío (núcleo 0): puede tardar lo que sea sin frenar el vuelo
bool flush() {
  if (!bufCount || WiFi.status() != WL_CONNECTED || !sendBatch) return false;

  xSemaphoreTake(bufMutex, portMAX_DELAY);
  int n = min(bufCount, BATCH_SIZE);
  for (int i = 0; i < n; i++) sendBatch[i] = buffer[(bufHead + i) % bufferCap];
  xSemaphoreGive(bufMutex);
  if (!n) return false;

  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  for (int i = 0; i < n; i++) toJson(arr.add<JsonObject>(), sendBatch[i]);
  String body;
  serializeJson(doc, body);

  if (!http.begin(tls, SERVER_URL)) return false;
  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-API-Key", API_KEY);
  uint32_t t0 = millis();
  int code = http.POST(body);
  String resp = code > 0 ? http.getString() : "";
  http.end();

  if (code == 201) {
    dropSent(sendBatch[n - 1].seq);
    lastSendOkMs = millis();
    digitalWrite(STATUS_LED_PIN, !digitalRead(STATUS_LED_PIN));
    Serial.printf("  Envío correcto: %d lectura(s) en %lu ms · pendientes: %d\n", n, millis() - t0, bufCount);
    return true;
  }

  if (code < 0) {
    char err[120] = "";
    tls.lastError(err, sizeof(err));
    IPAddress ip;
    bool dns = WiFi.hostByName(SERVER_HOST, ip);
    Serial.printf("  Error de conexión: %s · TLS: %s · DNS: %s · señal %d dBm\n",
                  HTTPClient::errorToString(code).c_str(), err,
                  dns ? ip.toString().c_str() : "FALLÓ", WiFi.RSSI());
    tls.stop();                          // la próxima vez abre una conexión nueva
    sendBackoffUntil = millis() + 3000;  // espera 3 s antes de reintentar (no traba las lecturas)
    char line[160];
    snprintf(line, sizeof(line), "# Error de envio (uptime %lu s): %s, TLS: %s, DNS: %s, senal %d dBm",
             (unsigned long)(millis() / 1000), HTTPClient::errorToString(code).c_str(), err,
             dns ? "ok" : "FALLO", WiFi.RSSI());
    netLogEvent(line);
  } else {
    Serial.printf("  Error HTTP %d: %s\n", code, resp.c_str());
    char line[120];
    snprintf(line, sizeof(line), "# Error HTTP %d (uptime %lu s)", code, (unsigned long)(millis() / 1000));
    netLogEvent(line);
  }
  sendErrors++;

  if (code == 400) dropSent(sendBatch[n - 1].seq);   // datos inválidos: se descartan para no trabar la cola
  return false;
}

// Tarea de envío: conecta el WiFi y manda lo pendiente, separada del vuelo
void netTask(void *) {
  for (;;) {
    if (bufCount && (int32_t)(millis() - sendBackoffUntil) >= 0) {
      connectWiFi();
      flush();
    }
    vTaskDelay(pdMS_TO_TICKS(SEND_INTERVAL_MS));
  }
}

// ================================================================ MONITOR SERIE

void printReading(const Reading &r) {
  Serial.printf("#%u  T=%.2f°C  P=%.2f hPa  Alt=%.1f m (rel %.1f, máx %.1f)",
                (unsigned)r.seq, r.temperature, r.pressure, r.altitude, r.altRel, r.maxAltRel);
  if (isfinite(r.battery)) Serial.printf("  Bat=%.2f V (%d%%)", r.battery, r.batteryPct);
  Serial.printf("  Paracaídas=%s", deployText(r.deploy));
  if (mpuOk) Serial.printf("  G=%.2f (máx %.2f)  incl=%.0f°/%.0f°", r.gForce, r.gMax, r.pitch, r.roll);
  Serial.println();

  if (r.batteryPct >= 0 && r.batteryPct < BATTERY_LOW_PCT)
    Serial.println("  ⚠ BATERÍA BAJA: cargar antes de lanzar");
}

// ================================================================ SETUP / LOOP

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n==================================");
  Serial.println("Titan ATLAS · Misión Domuyo · firmware v3.11");
  Serial.println("Telemetría 2 Hz + SD + paracaídas + batería");
  Serial.println("==================================");
  Serial.printf("Motivo del arranque: %s\n", resetReasonText());
  bootReason = bootReasonShort();

  // Código de arranque en el LED azul (sirve sin notebook ni SD):
  //   1 destello = encendido normal · 2 = BROWNOUT · 3 = error del programa · 4 = watchdog
  //   después queda fijo mientras inicia; cuando arranca el loop pasa al modo normal.
  //   Si los destellos se repiten cada pocos segundos, el ESP32 se está reiniciando.
  pinMode(STATUS_LED_PIN, OUTPUT);
  int blinks = bootBlinkCount();
  for (int i = 0; i < blinks; i++) {
    digitalWrite(STATUS_LED_PIN, HIGH); delay(120);
    digitalWrite(STATUS_LED_PIN, LOW);  delay(220);
  }
  delay(400);
  digitalWrite(STATUS_LED_PIN, HIGH);   // fijo mientras inicializa

  setenv("TZ", "UTC0", 1);
  tzset();
  sntp_set_time_sync_notification_cb(onNtpSync);   // antes de configTime()

  bufMutex = xSemaphoreCreateMutex();
  initBuffer();                // primero: reserva la memoria para guardar lecturas sin conexión
  sendBatch = (Reading *)malloc(sizeof(Reading) * BATCH_SIZE);
  initSD();                    // si falla, el resto del firmware sigue funcionando
  initBattery();
  initBmp();
  initMpu();
  initWiFi();
  initServos();                // al final: reparte los picos de corriente del arranque

  float v = readBattery();
  if (isfinite(v)) Serial.printf("Batería: %.2f V (%d%%)\n", v, batteryPercent(v));
  else Serial.println(BATTERY_PIN < 0 ? "Batería: medición desactivada (BATTERY_PIN -1)"
                                       : "Batería: divisor no conectado (no se mide)");

  // El envío al servidor corre aparte, en el núcleo 0 (el del WiFi): el loop queda libre
  xTaskCreatePinnedToCore(netTask, "envio", 10240, nullptr, 1, nullptr, 0);

  Serial.println("\nSistema iniciado (el paracaídas funciona aunque no haya WiFi ni internet)");
}

void loop() {
  uint32_t now = millis();

  // Acelerómetro a 50 Hz para registrar los picos de G
  if (now - lastImu >= IMU_INTERVAL_MS) {
    lastImu = now;
    readImu();
  }

  // Barómetro a 10 Hz: altura y control del paracaídas (independiente del WiFi)
  if (now - lastBaro >= BARO_INTERVAL_MS) {
    lastBaro = now;
    readBaro();
    updateDeploy();
  }

  // Si el BMP390 no responde, se lo vuelve a buscar cada 5 s
  if (!bmpOk && now - lastBmpTry >= 5000) {
    lastBmpTry = now;
    initBmp();
  }

  // Una lectura de telemetría cada 0,5 s, a ritmo fijo
  if (now - lastSample >= SAMPLE_INTERVAL_MS) {
    lastSample = (now - lastSample > 2 * SAMPLE_INTERVAL_MS) ? now : lastSample + SAMPLE_INTERVAL_MS;
    Reading r = sample();
    if (keepInBuffer(now)) pushBuffer(r);
    saveToSD(r);
    printReading(r);
  }

  // Cortes de WiFi: se anotan en la SD para poder revisarlos después de una prueba
  if (wifiDrops != wifiDropsLogged) {
    wifiDropsLogged = wifiDrops;
    char line[100];
    snprintf(line, sizeof(line), "# WiFi desconectado (uptime %lu s): motivo %u, cortes totales %lu",
             (unsigned long)(now / 1000), (unsigned)wifiLastReason, (unsigned long)wifiDropsLogged);
    sdLogEvent(line);
  }

  updateStatusLed(now);
  updateServoSequence(now);    // apertura del paracaídas en curso (si la hay)

  // SD: graba lo pendiente cada 2 s; desde que se arma, cada 0,5 s (así no se pierde la caída
  // si el golpe corta la alimentación)
  uint32_t sdEvery = deployState == DEPLOY_WAITING ? SD_FLUSH_MS : SAMPLE_INTERVAL_MS;
  if (sdOk && now - lastSdFlush >= sdEvery) {
    lastSdFlush = now;
    logFile.flush();
  }

  // Errores de envío: la tarea de envío los deja anotados y acá se graban en la SD
  if (netEvents != netEventsLogged) {
    char line[160];
    xSemaphoreTake(bufMutex, portMAX_DELAY);
    strlcpy(line, netEventText, sizeof(line));
    netEventsLogged = netEvents;
    xSemaphoreGive(bufMutex);
    sdLogEvent(line);
  }
  // (el envío al servidor lo hace netTask en el otro núcleo: este loop nunca espera al WiFi)
}
