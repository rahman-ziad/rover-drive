# Rover Manual Control (ESP32 + iBus + BTS7960)

Self-contained manual radio control firmware for an ESP32-based rover using a FlySky iBus receiver and 4 BTS7960 high-power H-bridge motor drivers (2 for skid-steer drive wheels and 2 for linear actuators).

> **Zero External Libraries Required**: This sketch features a built-in iBus protocol decoder and native ESP32 LEDC PWM controller. It compiles out-of-the-box on both ESP32 Arduino Core 2.x and Core 3.x with no library installation needed.

---

## Hardware Pinout (Preserved - Do Not Change)

### 1. iBus Receiver
| Receiver Signal | ESP32 GPIO Pin | Description |
|---|---|---|
| iBus Signal (IBUS / SERVO) | **GPIO 16** | HardwareSerial UART2 RX |
| VCC | 5V | Receiver Power |
| GND | GND | Common Ground |

### 2. BTS7960 Motor Drivers
Each BTS7960 driver requires Forward PWM, Reverse PWM, Power, and Enable pins:
- **R_EN** and **L_EN** on all 4 BTS7960 drivers should be connected to **5V** (or HIGH) to keep drivers enabled.
- **VCC** connected to **5V**, **GND** to **Common GND**.

| Driver | Function | RPWM Pin | LPWM Pin | LEDC Channels |
|---|---|---|---|---|
| **Left Drive** | Main Left Wheels | `LPWM` (**22**) | `LLPWM` (**23**) | CH 0, CH 1 |
| **Right Drive** | Main Right Wheels | `RRPWM` (**19**) | `RLPWM` (**21**) | CH 2, CH 3 |
| **Driver 3** | Linear Actuator 1 (M3) | `M3_RPWM` (**25**) | `M3_LPWM` (**26**) | CH 4, CH 5 |
| **Driver 4** | Linear Actuator 2 (M4) | `M4_RPWM` (**32**) | `M4_LPWM` (**33**) | CH 6, CH 7 |

---

## Radio Channel Mapping (FlySky iBus)

| Channel | Transmitter Control | Function | Expected Behavior |
|---|---|---|---|
| **CH 1** | Right Stick Horizontal | **Steering** | 1000 µs (Left) - 1500 µs (Center) - 2000 µs (Right) |
| **CH 2** | Right Stick Vertical | **Throttle** | 1000 µs (Reverse) - 1500 µs (Center) - 2000 µs (Forward) |
| **CH 4** | Left Stick Horizontal / Switch | **Linear Actuator 1** | **1500 µs Neutral / Stop**; < 1500 µs Retract; > 1500 µs Extend |
| **CH 5** | Aux Switch / Knob | **Linear Actuator 2** | **1500 µs Neutral / Stop**; < 1500 µs Retract; > 1500 µs Extend |

> **Linear Actuators**: Both CH4 and CH5 treat **1500 µs** as neutral (stopped). Moving the stick or switch in either direction from 1500 will drive the linear actuator in that direction.

---

## Configuration & Tuning Options

All tuning flags are at the top of [main.ino](main.ino):

- **Inversion Flags**:
  ```cpp
  const bool INVERT_LEFT_MOTOR  = false;
  const bool INVERT_RIGHT_MOTOR = false;
  const bool INVERT_STEER       = false;
  const bool INVERT_THROTTLE    = false;
  const bool INVERT_ACTUATOR_1  = false;
  const bool INVERT_ACTUATOR_2  = false;
  ```
  If any motor or stick moves backwards due to motor wiring polarity, change the flag to `true`.

- **Deadband**:
  - `STICK_DEADBAND` (default `30` µs): Eliminates buzzing/jitter when sticks rest at 1500 µs.
  - `ACTUATOR_DEADBAND` (default `50` µs): Ensures linear actuators stop firmly around 1500 µs.

- **Actuator Power**:
  - `ACTUATOR_SPEED` (default `255`): Sets maximum PWM duty.
  - `ACTUATOR_PROPORTIONAL` (default `false`): `false` delivers full rated power (255) when deflected outside deadband (recommended for DC worm-gear linear actuators to prevent motor stalling); `true` makes speed proportional to stick deflection.

- **Failsafe**:
  - `IBUS_TIMEOUT_MS` (default `200` ms): Automatically brings all 4 motor drivers to 0 PWM if the transmitter is turned off or radio connection is lost.

---

## How to Compile & Upload in Arduino IDE

1. Open [main.ino](main.ino) in Arduino IDE.
2. Select your ESP32 board (e.g., **ESP32 Dev Module**).
3. Click **Verify / Compile** (Ctrl+R / Cmd+R). No external libraries need to be installed.
4. Connect your ESP32 via USB and click **Upload** (Ctrl+U / Cmd+U).
5. Open Serial Monitor at **115200 baud** to view live channel diagnostics and motor output speeds.
