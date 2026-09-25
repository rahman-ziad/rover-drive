#include <Arduino.h>
#include <ESP32Servo.h>
#include "BluetoothSerial.h"
#include <Wire.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include <stdio.h>
#include <string.h>

BluetoothSerial SerialBT;
Adafruit_MPU6050 mpu;

// =====================================================
// PINS
// =====================================================

#define LEFT_ESC_PIN  25
#define RIGHT_ESC_PIN 26

Servo leftESC;
Servo rightESC;


// =====================================================
// ESC CONFIG
// =====================================================

const int ESC_STOP  = 1505;
const int ESC_SPEED = 125;

const float MAX_FORWARD_SECONDS = 3600.0;
const unsigned long CONTINUOUS_COMMAND_TIMEOUT = 1000;


// =====================================================
// MOTOR DIRECTION
// =====================================================

bool REVERSE_LEFT_MOTOR  = false;
bool REVERSE_RIGHT_MOTOR = false;


// Set these after reading the addresses printed by the ESP32.
// Leave an address empty when that device is not used.
const char* MOBILE_BT_ADDRESS = "";
const char* LAPTOP_BT_ADDRESS = "";


// =====================================================
// MPU CALIBRATION
// =====================================================

float gyroBiasX = 0;
float gyroBiasY = 0;
float gyroBiasZ = 0;

float accelBiasX = 0;
float accelBiasY = 0;
float accelBiasZ = 0;


// =====================================================
// MPU VALUES
// =====================================================

float rawGyroX = 0;
float rawGyroY = 0;
float rawGyroZ = 0;

float rawAccelX = 0;
float rawAccelY = 0;
float rawAccelZ = 0;

float accelX = 0;
float accelY = 0;
float accelZ = 0;

float gyroX = 0;
float gyroY = 0;
float gyroZ = 0;


// =====================================================
// ANGLE ROTATION
// =====================================================

bool angleRotationActive = false;

float targetAngle = 0;
float rotatedAngle = 0;

int rotationDirection = 0;
// +1 = right
// -1 = left

unsigned long lastAngleUpdate = 0;


// =====================================================
// NORMAL MOVEMENT
// =====================================================

bool timedMovementActive = false;
bool continuousMovementActive = false;

unsigned long movementStartTime = 0;
unsigned long movementDuration = 0;
unsigned long lastContinuousCommand = 0;


// =====================================================
// TELEMETRY
// =====================================================

unsigned long lastTelemetry = 0;

const unsigned long TELEMETRY_INTERVAL = 100;


// =====================================================
// BLUETOOTH COMMAND
// =====================================================

String command = "";

volatile bool bluetoothClientConnected = false;
volatile bool bluetoothStatusReportPending = false;
uint32_t bluetoothClientHandle = 0;
char bluetoothClientAddress[18] = "NONE";
char bluetoothClientType[8] = "UNKNOWN";


// =====================================================
// MOTOR OUTPUT
// =====================================================

int leftOutput(int value) {

  if (REVERSE_LEFT_MOTOR)
    value = -value;

  return constrain(ESC_STOP + value, 1000, 2000);
}


int rightOutput(int value) {

  if (REVERSE_RIGHT_MOTOR)
    value = -value;

  return constrain(ESC_STOP + value, 1000, 2000);
}


void setMotors(int left, int right) {

  leftESC.writeMicroseconds(leftOutput(left));
  rightESC.writeMicroseconds(rightOutput(right));
}


void stopMotors() {

  setMotors(0, 0);

  timedMovementActive = false;
  angleRotationActive = false;
  continuousMovementActive = false;

  rotatedAngle = 0;

  Serial.println("STOP");
  SerialBT.println("STOP");
}


// =====================================================
// MPU CALIBRATION
// =====================================================

void calibrateMPU() {

  Serial.println();
  Serial.println("==============================");
  Serial.println(" MPU6050 CALIBRATION");
  Serial.println("==============================");

  Serial.println("Keep the bot COMPLETELY STILL.");
  Serial.println("Calibration starts in 3 seconds...");

  delay(1000);

  Serial.println("2...");
  delay(1000);

  Serial.println("1...");
  delay(1000);

  Serial.println("Calibrating...");

  const int samples = 2000;

  float gx = 0;
  float gy = 0;
  float gz = 0;

  float ax = 0;
  float ay = 0;
  float az = 0;

  sensors_event_t a, g, temp;

  for (int i = 0; i < samples; i++) {

    mpu.getEvent(&a, &g, &temp);

    gx += g.gyro.x;
    gy += g.gyro.y;
    gz += g.gyro.z;

    ax += a.acceleration.x;
    ay += a.acceleration.y;
    az += a.acceleration.z;

    delay(2);
  }

  gyroBiasX = gx / samples;
  gyroBiasY = gy / samples;
  gyroBiasZ = gz / samples;

  accelBiasX = ax / samples;
  accelBiasY = ay / samples;
  accelBiasZ = az / samples;

  Serial.println();
  Serial.println("Calibration complete!");

  Serial.print("Gyro bias X: ");
  Serial.println(gyroBiasX, 6);

  Serial.print("Gyro bias Y: ");
  Serial.println(gyroBiasY, 6);

  Serial.print("Gyro bias Z: ");
  Serial.println(gyroBiasZ, 6);

  Serial.println("==============================");
  Serial.println();
}


// =====================================================
// READ MPU
// =====================================================

void updateMPU() {

  sensors_event_t a, g, temp;

  mpu.getEvent(&a, &g, &temp);

  // RAW
  rawAccelX = a.acceleration.x;
  rawAccelY = a.acceleration.y;
  rawAccelZ = a.acceleration.z;

  rawGyroX = g.gyro.x;
  rawGyroY = g.gyro.y;
  rawGyroZ = g.gyro.z;

  // CALIBRATED GYRO
  gyroX = rawGyroX - gyroBiasX;
  gyroY = rawGyroY - gyroBiasY;
  gyroZ = rawGyroZ - gyroBiasZ;

  // CALIBRATED ACCELEROMETER
  accelX = rawAccelX - accelBiasX;
  accelY = rawAccelY - accelBiasY;
  accelZ = rawAccelZ - accelBiasZ;
}


// =====================================================
// TELEMETRY
// =====================================================

void printTelemetry(Print& output) {

  output.print("ACC RAW: ");
  output.print(rawAccelX, 2);
  output.print("  ");
  output.print(rawAccelY, 2);
  output.print("  ");
  output.println(rawAccelZ, 2);

  output.print("ACC CAL: ");
  output.print(accelX, 2);
  output.print("  ");
  output.print(accelY, 2);
  output.print("  ");
  output.println(accelZ, 2);

  output.print("GYRO RAW: ");
  output.print(rawGyroX, 3);
  output.print("  ");
  output.print(rawGyroY, 3);
  output.print("  ");
  output.println(rawGyroZ, 3);

  output.print("GYRO CAL: ");
  output.print(gyroX, 3);
  output.print("  ");
  output.print(gyroY, 3);
  output.print("  ");
  output.println(gyroZ, 3);
}


void sendTelemetry() {

  if (millis() - lastTelemetry < TELEMETRY_INTERVAL)
    return;

  lastTelemetry = millis();

  printTelemetry(Serial);
  printTelemetry(SerialBT);
}


// =====================================================
// START CONTINUOUS MOVEMENT
// =====================================================

void startContinuousMovement(int left, int right, const char* label) {

  timedMovementActive = false;
  angleRotationActive = false;
  continuousMovementActive = true;
  lastContinuousCommand = millis();

  setMotors(left, right);

  Serial.println(label);
  SerialBT.println(label);
}


// =====================================================
// START FORWARD FOR A DURATION
// =====================================================

void moveForwardFor(float seconds) {

  if (seconds <= 0 || seconds > MAX_FORWARD_SECONDS) {

    Serial.print("Invalid F duration: ");
    Serial.println(seconds, 3);

    SerialBT.print("Invalid F duration: ");
    SerialBT.println(seconds, 3);

    return;
  }

  angleRotationActive = false;
  continuousMovementActive = false;

  setMotors(
    ESC_SPEED,
    ESC_SPEED
  );

  timedMovementActive = true;
  movementStartTime = millis();
  movementDuration = (unsigned long)(seconds * 1000.0);

  Serial.print("FORWARD ");
  Serial.print(seconds, 3);
  Serial.println(" sec");

  SerialBT.print("FORWARD ");
  SerialBT.print(seconds, 3);
  SerialBT.println(" sec");
}


void moveForward() {

  startContinuousMovement(
    ESC_SPEED,
    ESC_SPEED,
    "FORWARD"
  );
}


// =====================================================
// START BACKWARD WHILE HELD
// =====================================================

void moveBackward() {

  startContinuousMovement(
    -ESC_SPEED,
    -ESC_SPEED,
    "BACKWARD"
  );
}


// =====================================================
// ROTATE LEFT WHILE HELD
// =====================================================

void rotateLeft() {

  startContinuousMovement(
     ESC_SPEED,
    -ESC_SPEED,
    "ROTATE LEFT"
  );
}


// =====================================================
// ROTATE RIGHT WHILE HELD
// =====================================================

void rotateRight() {

  startContinuousMovement(
    -ESC_SPEED,
     ESC_SPEED,
    "ROTATE RIGHT"
  );
}


// =====================================================
// ROTATE EXACT ANGLE
// =====================================================

void rotateAngle(int direction, float degrees) {

  if (degrees <= 0)
    return;

  timedMovementActive = false;
  continuousMovementActive = false;

  rotationDirection = direction;

  targetAngle = degrees;
  rotatedAngle = 0;

  angleRotationActive = true;

  lastAngleUpdate = micros();

  if (direction < 0) {

    // LEFT
    setMotors(
       ESC_SPEED,
      -ESC_SPEED
    );

    Serial.print("ROTATE LEFT ");
    Serial.print(degrees);
    Serial.println(" deg");

    SerialBT.print("ROTATE LEFT ");
    SerialBT.print(degrees);
    SerialBT.println(" deg");

  } else {

    // RIGHT
    setMotors(
      -ESC_SPEED,
       ESC_SPEED
    );

    Serial.print("ROTATE RIGHT ");
    Serial.print(degrees);
    Serial.println(" deg");

    SerialBT.print("ROTATE RIGHT ");
    SerialBT.print(degrees);
    SerialBT.println(" deg");
  }
}


// =====================================================
// UPDATE ANGLE ROTATION
// =====================================================

void updateAngleRotation() {

  if (!angleRotationActive)
    return;

  unsigned long now = micros();

  float dt = (now - lastAngleUpdate) / 1000000.0;

  lastAngleUpdate = now;

  // gyroZ is rad/s
  // convert to degrees/s

  float gyroDegPerSec = gyroZ * 57.2957795;

  // Accumulate absolute rotation

  rotatedAngle += abs(gyroDegPerSec * dt);

  if (rotatedAngle >= targetAngle) {

    stopMotors();

    Serial.print("ANGLE COMPLETE: ");
    Serial.print(rotatedAngle, 1);
    Serial.println(" deg");

    SerialBT.print("ANGLE COMPLETE: ");
    SerialBT.print(rotatedAngle, 1);
    SerialBT.println(" deg");
  }
}


// =====================================================
// UPDATE TIMED MOVEMENT
// =====================================================

void updateTimedMovement() {

  if (!timedMovementActive)
    return;

  if (millis() - movementStartTime >= movementDuration) {

    stopMotors();
  }
}


// =====================================================
// UPDATE CONTINUOUS MOVEMENT
// =====================================================

void updateContinuousMovement() {

  if (!continuousMovementActive)
    return;

  if (millis() - lastContinuousCommand >= CONTINUOUS_COMMAND_TIMEOUT) {

    stopMotors();
  }
}


// =====================================================
// BLUETOOTH CLIENT STATUS
// =====================================================

char uppercaseBluetoothChar(char value) {

  if (value >= 'a' && value <= 'z')
    return value - ('a' - 'A');

  return value;
}


bool bluetoothAddressMatches(const char* actual, const char* configured) {

  if (configured == nullptr || configured[0] == '\0')
    return false;

  for (int i = 0; i < 17; i++) {

    if (actual[i] == '\0' || configured[i] == '\0')
      return false;

    if (uppercaseBluetoothChar(actual[i]) != uppercaseBluetoothChar(configured[i]))
      return false;
  }

  return actual[17] == '\0' && configured[17] == '\0';
}


void formatBluetoothAddress(const uint8_t* address, char* output, size_t outputSize) {

  if (outputSize < 18)
    return;

  snprintf(
    output,
    outputSize,
    "%02X:%02X:%02X:%02X:%02X:%02X",
    address[0],
    address[1],
    address[2],
    address[3],
    address[4],
    address[5]
  );
}


const char* bluetoothClientTypeForAddress(const char* address) {

  if (bluetoothAddressMatches(address, MOBILE_BT_ADDRESS))
    return "MOBILE";

  if (bluetoothAddressMatches(address, LAPTOP_BT_ADDRESS))
    return "LAPTOP";

  return "UNKNOWN";
}


void printBluetoothClientStatus(bool connected) {

  if (connected) {

    Serial.println("BT STATUS: CONNECTED");
    SerialBT.println("BT STATUS: CONNECTED");

    Serial.print("BT CLIENT TYPE: ");
    Serial.println(bluetoothClientType);
    SerialBT.print("BT CLIENT TYPE: ");
    SerialBT.println(bluetoothClientType);

    Serial.print("BT CLIENT ADDRESS: ");
    Serial.println(bluetoothClientAddress);
    SerialBT.print("BT CLIENT ADDRESS: ");
    SerialBT.println(bluetoothClientAddress);

  } else {

    Serial.println("BT STATUS: DISCONNECTED");
    Serial.println("BT CLIENT TYPE: NONE");
    Serial.println("BT CLIENT ADDRESS: NONE");

    SerialBT.println("BT STATUS: DISCONNECTED");
    SerialBT.println("BT CLIENT TYPE: NONE");
    SerialBT.println("BT CLIENT ADDRESS: NONE");
  }
}


void reportPendingBluetoothStatus() {

  if (!bluetoothStatusReportPending)
    return;

  bluetoothStatusReportPending = false;
  printBluetoothClientStatus(bluetoothClientConnected);
}


void bluetoothSPPCallback(esp_spp_cb_event_t event, esp_spp_cb_param_t* param) {

  if (param == nullptr)
    return;

  if (event == ESP_SPP_SRV_OPEN_EVT) {

    if (param->srv_open.status != ESP_SPP_SUCCESS)
      return;

    char incomingAddress[18];
    formatBluetoothAddress(
      param->srv_open.rem_bda,
      incomingAddress,
      sizeof(incomingAddress)
    );

    if (bluetoothClientConnected) {

      Serial.print("BT CLIENT REJECTED: ");
      Serial.print(bluetoothClientTypeForAddress(incomingAddress));
      Serial.print(" ");
      Serial.print(incomingAddress);
      Serial.println("; another client is already connected");
      return;
    }

    strncpy(
      bluetoothClientAddress,
      incomingAddress,
      sizeof(bluetoothClientAddress) - 1
    );
    bluetoothClientAddress[sizeof(bluetoothClientAddress) - 1] = '\0';

    strncpy(
      bluetoothClientType,
      bluetoothClientTypeForAddress(bluetoothClientAddress),
      sizeof(bluetoothClientType) - 1
    );
    bluetoothClientType[sizeof(bluetoothClientType) - 1] = '\0';

    bluetoothClientHandle = param->srv_open.handle;
    bluetoothClientConnected = true;
    bluetoothStatusReportPending = true;
  }

  else if (event == ESP_SPP_CLOSE_EVT) {

    if (!bluetoothClientConnected)
      return;

    if (param->close.handle != bluetoothClientHandle)
      return;

    bluetoothClientConnected = false;
    bluetoothClientHandle = 0;
    strcpy(bluetoothClientAddress, "NONE");
    strcpy(bluetoothClientType, "UNKNOWN");
    bluetoothStatusReportPending = true;
  }
}


// =====================================================
// PROCESS COMMAND
// =====================================================

void processCommand(String cmd) {

  cmd.trim();
  cmd.toUpperCase();

  if (cmd.length() == 0)
    return;

  Serial.print("Command: ");
  Serial.println(cmd);

  SerialBT.print("Command: ");
  SerialBT.println(cmd);


  // ---------------------------------------------
  // F = continuous forward, F<number> = timed forward
  // ---------------------------------------------

  if (cmd.startsWith("F")) {

    String secondsString = cmd.substring(1);

    if (secondsString.length() == 0) {

      moveForward();

    } else {

      moveForwardFor(secondsString.toFloat());
    }
  }


  // ---------------------------------------------
  // B
  // ---------------------------------------------

  else if (cmd == "B") {

    moveBackward();
  }


  // ---------------------------------------------
  // L
  // ---------------------------------------------

  else if (cmd == "L") {

    rotateLeft();
  }


  // ---------------------------------------------
  // R
  // ---------------------------------------------

  else if (cmd == "R") {

    rotateRight();
  }


  // ---------------------------------------------
  // STOP
  // ---------------------------------------------

  else if (cmd == "S") {

    stopMotors();
  }


  // ---------------------------------------------
  // RL90
  // RL45
  // RL180
  // ---------------------------------------------

  else if (cmd.startsWith("RL")) {

    String angleString = cmd.substring(2);

    float degrees = angleString.toFloat();

    if (degrees > 0) {

      rotateAngle(-1, degrees);

    } else {

      Serial.println("Invalid RL angle");
      SerialBT.println("Invalid RL angle");
    }
  }


  // ---------------------------------------------
  // RR90
  // RR45
  // RR180
  // ---------------------------------------------

  else if (cmd.startsWith("RR")) {

    String angleString = cmd.substring(2);

    float degrees = angleString.toFloat();

    if (degrees > 0) {

      rotateAngle(1, degrees);

    } else {

      Serial.println("Invalid RR angle");
      SerialBT.println("Invalid RR angle");
    }
  }


  // ---------------------------------------------
  // UNKNOWN
  // ---------------------------------------------

  else {

    Serial.println("Unknown command");

    SerialBT.println("Unknown command");
  }
}


// =====================================================
// BLUETOOTH INPUT
// =====================================================

void readBluetooth() {

  while (SerialBT.available()) {

    char c = SerialBT.read();

    if (c == '\n' || c == '\r') {

      if (command.length() > 0) {

        processCommand(command);

        command = "";
      }
    }

    else {

      command += c;
    }
  }
}


// =====================================================
// USB SERIAL INPUT  (commands from Jetson Orin)
// =====================================================

String serialCommand = "";

void readSerial() {

  while (Serial.available()) {

    char c = (char)Serial.read();

    if (c == '\n' || c == '\r') {

      if (serialCommand.length() > 0) {

        processCommand(serialCommand);

        serialCommand = "";
      }
    }

    else {

      serialCommand += c;
    }
  }
}


// =====================================================
// SETUP
// =====================================================

void setup() {

  Serial.begin(115200);

  delay(500);


  // =================================================
  // ESC
  // =================================================

  leftESC.setPeriodHertz(50);
  rightESC.setPeriodHertz(50);

  leftESC.attach(
    LEFT_ESC_PIN,
    1000,
    2000
  );

  rightESC.attach(
    RIGHT_ESC_PIN,
    1000,
    2000
  );


  // IMPORTANT
  // Neutral = 1505

  setMotors(0, 0);

  delay(3000);


  // =================================================
  // MPU6050
  // =================================================

  Wire.begin();

  if (!mpu.begin()) {

    Serial.println("MPU6050 NOT FOUND!");

    while (1) {

      setMotors(0, 0);

      delay(1000);
    }
  }

  Serial.println("MPU6050 FOUND");


  mpu.setAccelerometerRange(MPU6050_RANGE_8_G);

  mpu.setGyroRange(MPU6050_RANGE_500_DEG);

  mpu.setFilterBandwidth(MPU6050_BAND_21_HZ);


  // =================================================
  // CALIBRATE
  // =================================================

  calibrateMPU();


  // =================================================
  // BLUETOOTH
  // =================================================

  //SerialBT.register_callback(bluetoothSPPCallback);
  bool bluetoothInitialized = SerialBT.begin("MPU6050_BOT");

  if (!bluetoothInitialized) {

    Serial.println("Bluetooth initialization failed");

  } else {

    Serial.println("Bluetooth initialized");
  }


  Serial.println();
  Serial.println("==============================");
  Serial.println("       BOT READY");
  Serial.println("==============================");

  Serial.println("Bluetooth: MPU6050_BOT");
  if (bluetoothInitialized) {
    Serial.print("ESP32 Bluetooth address: ");
    Serial.println(SerialBT.getBtAddressString());
  }
  Serial.println("Mobile address: set MOBILE_BT_ADDRESS in the sketch");
  Serial.println("Laptop address: set LAPTOP_BT_ADDRESS in the sketch");

  Serial.println();
  Serial.println("Commands:");

  Serial.println("F       = Forward while held");
  Serial.println("F<number> = Forward for number of seconds");
  Serial.println("B       = Backward while held");
  Serial.println("L       = Left while held");
  Serial.println("R       = Right while held");
  Serial.println("S       = Stop");

  Serial.println();
  Serial.println("RL90    = Rotate left 90 deg");
  Serial.println("RR90    = Rotate right 90 deg");

  Serial.println();
  Serial.println("MPU telemetry: 100 ms");
  Serial.println("BT hold timeout: 1000 ms");
  Serial.println("ESC neutral: 1505");
  Serial.println("Speed: 125");

  Serial.println();
  Serial.println("READY");

  SerialBT.println("BOT READY");
  SerialBT.println("F/F<number>/B/L/R/S");
  SerialBT.println("RL90 / RR90");
  SerialBT.println("MPU telemetry: 100ms");
  SerialBT.println("BT hold timeout: 1000ms");
}


// =====================================================
// LOOP
// =====================================================

void loop() {

  // ---------------------------------------------
  // MPU ALWAYS RUNS
  // ---------------------------------------------

  updateMPU();

  reportPendingBluetoothStatus();


  // ---------------------------------------------
  // SEND TELEMETRY
  // ---------------------------------------------

  sendTelemetry();


  // ---------------------------------------------
  // RECEIVE BLUETOOTH
  // ---------------------------------------------

  readBluetooth();


  // ---------------------------------------------
  // RECEIVE USB SERIAL  (commands from Jetson Orin)
  // ---------------------------------------------

  readSerial();


  // ---------------------------------------------
  // UPDATE MOVEMENT
  // ---------------------------------------------

  updateTimedMovement();

  updateContinuousMovement();

  updateAngleRotation();
}
