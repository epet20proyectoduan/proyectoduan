
#include <Wire.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BMP3XX.h>
#include <ESP32Servo.h>
#include <math.h>

// ---------------- PINS ----------------
#define I2C_SDA       21
#define I2C_SCL       22

#define SERVO1_PIN    25
#define SERVO2_PIN    2

// ---------------- CONFIGURATION ----------------
#define ALTITUDE_THRESHOLD_M  15.0f
#define BASELINE_SAMPLES      30
#define INTERVAL_MS           250

// ---------------- OBJECTS ----------------
Adafruit_BMP3XX bmp;
Servo servo1;
Servo servo2;

// Initial pressure in Pa
float baselinePressurePa = 0.0f;

bool thresholdDetected = false;
unsigned long lastReading = 0;

// ---------------- RELATIVE ALTITUDE ----------------
float calculateRelativeAltitude(float pressurePa) {
  if (baselinePressurePa <= 0.0f || pressurePa <= 0.0f) {
    return NAN;
  }

  return 44330.0f *
         (1.0f - powf(pressurePa / baselinePressurePa, 0.19029495f));
}

// ---------------- INITIAL CALIBRATION ----------------
bool calibrateInitialPressure() {
  double pressureSum = 0.0;
  int validReadings = 0;

  Serial.println();
  Serial.println("Calibrating initial pressure...");
  Serial.println("Keep the BMP390 still at ambient pressure.");

  for (int i = 0; i < BASELINE_SAMPLES; i++) {
    if (bmp.performReading() && bmp.pressure > 0.0f) {
      pressureSum += bmp.pressure;
      validReadings++;
    }

    delay(100);
  }

  if (validReadings < BASELINE_SAMPLES * 0.8f) {
    Serial.println("ERROR: Not enough valid readings.");
    return false;
  }

  baselinePressurePa = pressureSum / validReadings;

  Serial.print("Initial pressure: ");
  Serial.print(baselinePressurePa / 100.0f, 2);
  Serial.println(" hPa");

  Serial.println("Initial relative altitude: 0 m");
  return true;
}

// ---------------- MANUAL SERVO CONTROL ----------------
void showHelp() {
  Serial.println();
  Serial.println("Available commands:");
  Serial.println("  1 = Servo 1 to 0 degrees");
  Serial.println("  2 = Servo 1 to 90 degrees");
  Serial.println("  3 = Servo 2 to 0 degrees");
  Serial.println("  4 = Servo 2 to 90 degrees");
  Serial.println("  a = Both servos to 0 degrees");
  Serial.println("  b = Both servos to 90 degrees");
  Serial.println("  h = Show this help");
  Serial.println();
  Serial.println("The 15 m threshold only generates a serial message.");
}

void processCommand(char command) {
  switch (command) {
    case '1':
      servo1.write(0);
      Serial.println("Servo 1: command to 0 degrees");
      break;

    case '2':
      servo1.write(90);
      Serial.println("Servo 1: command to 90 degrees");
      break;

    case '3':
      servo2.write(0);
      Serial.println("Servo 2: command to 0 degrees");
      break;

    case '4':
      servo2.write(90);
      Serial.println("Servo 2: command to 90 degrees");
      break;

    case 'a':
    case 'A':
      servo1.write(0);
      servo2.write(0);
      Serial.println("Both servos: command to 0 degrees");
      break;

    case 'b':
    case 'B':
      servo1.write(90);
      servo2.write(90);
      Serial.println("Both servos: command to 90 degrees");
      break;

    case 'h':
    case 'H':
      showHelp();
      break;

    case '\r':
    case '\n':
    case ' ':
      break;

    default:
      Serial.println("Comando desconocido.");
      break;
  }
}

// ---------------- SETUP ----------------
void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println();
  Serial.println("==================================");
  Serial.println("TITAN ATLAS - TEST :3");
  Serial.println("BMP390 + 2 MG90S servos");
  Serial.println("Modo simulacion");
  Serial.println("==================================");

  // Initialize I2C using the original circuit pins
  Wire.begin(I2C_SDA, I2C_SCL);

  // Initialize BMP390 over I2C
  if (!bmp.begin_I2C()) {
    Serial.println("ERROR: BMP390 no detectado.");
    Serial.println("Fijarse en conexiones de fuente, SDA, SCL y I2C.");

    while (true) {
      delay(1000);
    }
  }

  bmp.setTemperatureOversampling(BMP3_OVERSAMPLING_8X);
  bmp.setPressureOversampling(BMP3_OVERSAMPLING_4X);
  bmp.setIIRFilterCoeff(BMP3_IIR_FILTER_COEFF_3);
  bmp.setOutputDataRate(BMP3_ODR_25_HZ);

  Serial.println("BMP390 detectado bien");

  // Initialize servos
  servo1.setPeriodHertz(50);
  servo2.setPeriodHertz(50);

  bool servo1OK = servo1.attach(SERVO1_PIN, 1000, 2000);
  bool servo2OK = servo2.attach(SERVO2_PIN, 1000, 2000);

  if (!servo1OK || !servo2OK) {
    Serial.println("ERROR: no se pudo inicializar uno o dos de los servos");
    Serial.println("Checkear pins, fuente o librerias");

    while (true) {
      delay(1000);
    }
  }

  // Initial test position
  servo1.write(0);
  servo2.write(0);

  Serial.println("Servos inicializados");
  Serial.println("Comandos manuales disponibles");

  showHelp();

  // Establish the altitude reference
  if (!calibrateInitialPressure()) {
    Serial.println("No se puede calcular la altitud");
    while (true) {
      delay(1000);
    }
  }

  Serial.println();
  Serial.println("Prueba Iniciada");
}

// ---------------- MAIN LOOP ----------------
void loop() {
  // Read commands from Serial Monitor
  while (Serial.available() > 0) {
    processCommand((char)Serial.read());
  }

  // Update readings at regular intervals
  unsigned long currentTime = millis();

  if (currentTime - lastReading < INTERVAL_MS) {
    return;
  }

  lastReading = currentTime;

  if (!bmp.performReading()) {
    Serial.println("ERROR: fallo la lectura del BMP390.");
    return;
  }

  float pressurePa = bmp.pressure;
  float temperatureC = bmp.temperature;
  float altitudeM = calculateRelativeAltitude(pressurePa);

  if (!isfinite(altitudeM)) {
    Serial.println("ERROR: Altitud Invalida.");
    return;
  }

  Serial.print("Presion: ");
  Serial.print(pressurePa / 100.0f, 2);
  Serial.print(" hPa | Temperatura: ");
  Serial.print(temperatureC, 2);
  Serial.print(" C | Altitud Relativa: ");
  Serial.print(altitudeM, 2);
  Serial.println(" m");

  // SIMULATION: detect and report, without automatic actuation
  if (!thresholdDetected && altitudeM > ALTITUDE_THRESHOLD_M) {
    thresholdDetected = true;

    Serial.println();
    Serial.println("==================================");
    Serial.println("EVENTO SIMULADO: algo excedido");
    Serial.println("La altitud relativa esta sobre los 15m");
    Serial.println("Los servos no fueron automatizados aun");
    Serial.println("==================================");
    Serial.println();
  }
}
