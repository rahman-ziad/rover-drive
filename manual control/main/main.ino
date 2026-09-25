#include <Arduino.h>
#include <IBusBM.h>

// =====================================================
// 1. PIN DEFINITIONS (EXACT - UNCHANGED)
// =====================================================
#define IBUS_RX_PIN 16

// BTS7960 Motor Driver Pins
// Main Drive - Right Motor
#define RRPWM 19
#define RLPWM 21

// Main Drive - Left Motor
#define LPWM  22
#define LLPWM 23

// Linear Actuator 1 (M3)
#define M3_RPWM 25
#define M3_LPWM 26

// Linear Actuator 2 (M4)
#define M4_RPWM 32
#define M4_LPWM 33

// =====================================================
// 2. LEDC PWM CONFIGURATION 
// =====================================================
#define LPWM_CH     0
#define LLPWM_CH    1
#define RPWM_CH     2
#define RLPWM_CH    3
#define M3_RPWM_CH  4
#define M3_LPWM_CH  5
#define M4_RPWM_CH  6
#define M4_LPWM_CH  7

#define PWM_FREQ 1000  // 1 kHz
#define PWM_RES  8     // 8-bit resolution (0 - 255)

// Dual-core helper to support both ESP32 Arduino Core 2.x and 3.x
#if defined(ESP_ARDUINO_VERSION_MAJOR) && (ESP_ARDUINO_VERSION_MAJOR >= 3)
  inline void pwmSetup(uint8_t pin, uint8_t ch) {
    ledcAttachChannel(pin, PWM_FREQ, PWM_RES, ch);
  }
  inline void pwmWrite(uint8_t pin, uint8_t ch, uint32_t duty) {
    ledcWrite(pin, duty);
  }
#else
  inline void pwmSetup(uint8_t pin, uint8_t ch) {
    ledcSetup(ch, PWM_FREQ, PWM_RES);
    ledcAttachPin(pin, ch);
  }
  inline void pwmWrite(uint8_t pin, uint8_t ch, uint32_t duty) {
    ledcWrite(ch, duty);
  }
#endif

// =====================================================
// 3. IBUS CHANNELS & TUNING
// =====================================================
// iBus channel indices (0-based: Channel 1 -> 0, Channel 2 -> 1, ...)
#define CH1_STEER_IDX     0  // CH1: Steering
#define CH2_THROTTLE_IDX  1  // CH2: Throttle
#define CH4_LA1_IDX       3  // CH4: Linear Actuator 1
#define CH5_LA2_IDX       4  // CH5: Linear Actuator 2

// Deadband & Timeout Settings
#define IBUS_TIMEOUT_MS     200   // Failsafe timeout in ms
#define STICK_DEADBAND       30   // Deadband for drive sticks around 1500 us
#define ACTUATOR_DEADBAND    50   // Deadband for linear actuators around 1500 us

// Actuator Speed Settings
#define ACTUATOR_SPEED       255  // 1 to 255 (full speed recommended for linear actuators)
#define ACTUATOR_PROPORTIONAL false // false = full speed on deflection, true = proportional

// Inversion Flags (change to true if any motor moves the wrong way)
const bool INVERT_LEFT_MOTOR  = false;
const bool INVERT_RIGHT_MOTOR = false;
const bool INVERT_STEER       = false;
const bool INVERT_THROTTLE    = false;
const bool INVERT_ACTUATOR_1  = false;
const bool INVERT_ACTUATOR_2  = false;

// Debugging
#define DEBUG_INTERVAL_MS 200     // Telemetry print rate (0 = disabled)

// =====================================================
// 4. BTS7960 MOTOR DRIVER HELPER
// =====================================================
struct BTS7960Driver {
  uint8_t fwdPin;
  uint8_t revPin;
  uint8_t fwdCh;
  uint8_t revCh;
  bool inverted;

  void init() {
    pinMode(fwdPin, OUTPUT);
    pinMode(revPin, OUTPUT);
    digitalWrite(fwdPin, LOW);
    digitalWrite(revPin, LOW);
    pwmSetup(fwdPin, fwdCh);
    pwmSetup(revPin, revCh);
    stop();
  }

  void setSpeed(int speed) {
    if (inverted) speed = -speed;
    speed = constrain(speed, -255, 255);

    if (speed > 0) {
      pwmWrite(revPin, revCh, 0);            // Turn off reverse first (prevent shoot-through)
      pwmWrite(fwdPin, fwdCh, (uint32_t)speed);
    } else if (speed < 0) {
      pwmWrite(fwdPin, fwdCh, 0);            // Turn off forward first
      pwmWrite(revPin, revCh, (uint32_t)(-speed));
    } else {
      stop();
    }
  }

  void stop() {
    pwmWrite(fwdPin, fwdCh, 0);
    pwmWrite(revPin, revCh, 0);
  }
};

// Motor driver instances
BTS7960Driver leftMotor  = {LPWM,    LLPWM,   LPWM_CH,    LLPWM_CH,   INVERT_LEFT_MOTOR};
BTS7960Driver rightMotor = {RRPWM,   RLPWM,   RPWM_CH,    RLPWM_CH,   INVERT_RIGHT_MOTOR};
BTS7960Driver actuator1  = {M3_RPWM, M3_LPWM, M3_RPWM_CH, M3_LPWM_CH, INVERT_ACTUATOR_1};
BTS7960Driver actuator2  = {M4_RPWM, M4_LPWM, M4_RPWM_CH, M4_LPWM_CH, INVERT_ACTUATOR_2};

void stopAllMotors() {
  leftMotor.stop();
  rightMotor.stop();
  actuator1.stop();
  actuator2.stop();
}

// =====================================================
// 5. HARDWARE & IBUS LOGIC
// =====================================================
HardwareSerial IBusSerial(2);
IBusBM IBus;

uint8_t  lastFrameCount = 0;
uint32_t lastFrameTime  = 0;
uint32_t lastDebugTime  = 0;

int readChannel(uint8_t chIndex, int defaultValue = 1500) {
  uint16_t val = IBus.readChannel(chIndex);
  if (val < 800 || val > 2200) return defaultValue;
  return (int)val;
}

bool isIbusAlive() {
  uint8_t currentCount = IBus.cnt_rec;
  if (currentCount != lastFrameCount) {
    lastFrameCount = currentCount;
    lastFrameTime  = millis();
  }
  return (millis() - lastFrameTime) < IBUS_TIMEOUT_MS;
}

// Map pulse (1000 - 2000 us, center 1500) to speed (-255 to 255)
int pulseToDriveSpeed(int pulse, int deadband, bool invert) {
  int delta = pulse - 1500;
  if (abs(delta) <= deadband) return 0;

  int speed = 0;
  if (delta > 0) {
    speed = map(delta, deadband, 500, 0, 255);
  } else {
    speed = map(delta, -deadband, -500, 0, -255);
  }
  speed = constrain(speed, -255, 255);
  return invert ? -speed : speed;
}

// Actuator control: 1500 neutral, moving in either dir applies motion
int computeActuatorSpeed(int pulse, int deadband, bool invert) {
  int delta = pulse - 1500;
  if (abs(delta) <= deadband) return 0;

  int speed = 0;
  if (ACTUATOR_PROPORTIONAL) {
    if (delta > 0) {
      speed = map(delta, deadband, 500, 60, ACTUATOR_SPEED);
    } else {
      speed = map(delta, -deadband, -500, -60, -ACTUATOR_SPEED);
    }
    speed = constrain(speed, -ACTUATOR_SPEED, ACTUATOR_SPEED);
  } else {
    speed = (delta > 0) ? ACTUATOR_SPEED : -ACTUATOR_SPEED;
  }

  return invert ? -speed : speed;
}

void debugPrint(int steer, int throttle, int la1, int la2, int leftSpd, int rightSpd, int act1Spd, int act2Spd) {
#if DEBUG_INTERVAL_MS > 0
  if (millis() - lastDebugTime < DEBUG_INTERVAL_MS) return;
  lastDebugTime = millis();

  Serial.printf("CH:[ST:%4d TH:%4d LA1:%4d LA2:%4d] | DRIVE:[L:%+4d R:%+4d] | LA:[1:%+4d 2:%+4d]\n",
                steer, throttle, la1, la2, leftSpd, rightSpd, act1Spd, act2Spd);
#endif
}

// =====================================================
// 6. SETUP
// =====================================================
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n=== Rover Manual Control Starting ===");

  // Initialize motor drivers
  leftMotor.init();
  rightMotor.init();
  actuator1.init();
  actuator2.init();

  // Initialize iBus on UART2 RX (GPIO 16)
  IBusSerial.begin(115200, SERIAL_8N1, IBUS_RX_PIN, -1);
  IBus.begin(IBusSerial);

  lastFrameTime  = millis();
  lastFrameCount = IBus.cnt_rec;

  Serial.println("System Ready. Listening for iBus...");
}

// =====================================================
// 7. MAIN LOOP
// =====================================================
void loop() {
  // Failsafe: stop all motors immediately if transmitter is offline or disconnected
  if (!isIbusAlive()) {
    stopAllMotors();
    return;
  }

  // 1. Read Channels (default to 1500 neutral if invalid)
  int steerRaw    = readChannel(CH1_STEER_IDX,    1500);
  int throttleRaw = readChannel(CH2_THROTTLE_IDX, 1500);
  int la1Raw      = readChannel(CH4_LA1_IDX,      1500);
  int la2Raw      = readChannel(CH5_LA2_IDX,      1500);

  // 2. Drive Motors - Differential Steering Mixing
  int steer    = pulseToDriveSpeed(steerRaw,    STICK_DEADBAND, INVERT_STEER);
  int throttle = pulseToDriveSpeed(throttleRaw, STICK_DEADBAND, INVERT_THROTTLE);

  int leftSpeed  = throttle + steer;
  int rightSpeed = throttle - steer;

  // Normalize speeds to preserve turning authority at full speed
  int maxMagnitude = max(abs(leftSpeed), abs(rightSpeed));
  if (maxMagnitude > 255) {
    leftSpeed  = (leftSpeed * 255) / maxMagnitude;
    rightSpeed = (rightSpeed * 255) / maxMagnitude;
  }

  // 3. Linear Actuators - 1500 neutral, moving towards any dir applies that
  int act1Speed = computeActuatorSpeed(la1Raw, ACTUATOR_DEADBAND, INVERT_ACTUATOR_1);
  int act2Speed = computeActuatorSpeed(la2Raw, ACTUATOR_DEADBAND, INVERT_ACTUATOR_2);

  // 4. Output to Motor Drivers
  leftMotor.setSpeed(leftSpeed);
  rightMotor.setSpeed(rightSpeed);
  actuator1.setSpeed(act1Speed);
  actuator2.setSpeed(act2Speed);

  // 5. Diagnostics Output
  debugPrint(steerRaw, throttleRaw, la1Raw, la2Raw, leftSpeed, rightSpeed, act1Speed, act2Speed);
}
