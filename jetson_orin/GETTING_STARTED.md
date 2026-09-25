# Rover connection and startup guide

## 1. Understand the connection layout

The rover uses two separate links:

```text
phone  ─────── Bluetooth SPP/RFCOMM ────────►  ESP32  (direct phone control)
browser ── Wi-Fi/HTTP ──► Orin server ── USB-UART ──► ESP32  (web control)
```

The **Orin connects to the ESP32 via a USB cable** (not Bluetooth).  The ESP32
also advertises Bluetooth for a phone to connect directly — but that is separate
from the web-control path.

The Orin server does not create a Wi-Fi hotspot.  Put the Orin and control
device on the same router or existing hotspot.

---

## 2. Prepare and upload the ESP32 sketch

1. Open `mpu_zero/mpu_zero.ino` in Arduino IDE.
2. Select the correct ESP32 board and the USB port the ESP32 is connected to.
3. Install these libraries if they are not already installed:
   - ESP32Servo
   - Adafruit MPU6050
   - Adafruit Unified Sensor
4. Keep the wheels off the ground.
5. Upload the sketch.
6. Open the USB Serial Monitor at **115200 baud**.
7. Leave the ESP32 still during the three-second calibration countdown.

The serial monitor will print the telemetry stream:

```text
ACC RAW:  x y z
ACC CAL:  x y z
GYRO RAW: x y z
GYRO CAL: x y z
```

The calibrated values are the current readings minus the startup average.
Keep the sensor still while calibration runs.

---

## 3. Install Python on the Orin

Copy the `jetson_orin` folder to the Orin (or clone the repo there), then run
from inside it:

```bash
cd ~/rover/jetson_orin
sudo apt update
sudo apt install -y python3-pip python3-venv
python3 -m venv .venv
. .venv/bin/activate
pip install -r requirements.txt
```

### Give yourself serial-port permission

```bash
sudo usermod -aG dialout "$USER"
```

> **Log out and back in** (or reboot) after this command.  Without it, Python
> will get a *Permission denied* error when opening `/dev/ttyUSB0` or
> `/dev/ttyACM0`.

---

## 4. Connect the ESP32 to the Orin via USB

Plug a USB cable between the Orin and the ESP32.  Confirm the device appears:

```bash
ls /dev/ttyUSB* /dev/ttyACM* 2>/dev/null
```

You should see something like `/dev/ttyUSB0` or `/dev/ttyACM0`.

If nothing appears, try a different cable (some cables are charge-only with no
data lines).

---

## 5. Test the serial connection (optional)

You can verify the ESP32 is talking before starting the server:

```bash
python3 -c "
import serial, time
s = serial.Serial('/dev/ttyUSB0', 115200, timeout=2)
time.sleep(1)
for _ in range(10):
    print(s.readline().decode(errors='replace').strip())
s.close()
"
```

Replace `/dev/ttyUSB0` with the actual device if different.  You should see the
`ACC RAW:` / `GYRO RAW:` telemetry lines.

---

## 6. Start the Orin web server

```bash
. .venv/bin/activate
python rover_server.py
```

The server **auto-detects** the first `/dev/ttyUSB*` or `/dev/ttyACM*` and
saves the port to `~/.config/rover_bridge/config.json`.  On the next restart it
reconnects to the same port automatically.

To specify the port explicitly:

```bash
python rover_server.py --serial-port /dev/ttyACM0
```

Expected output:

```text
Serial rover connected on /dev/ttyUSB0
Rover server listening on http://0.0.0.0:8080/
```

---

## 7. Open the web UI

Find the Orin's IP address:

```bash
hostname -I
```

From any device on the same Wi-Fi network, open:

```text
http://ORIN_IP:8080/
```

The dashboard shows:

- **Serial link** — USB connection state and port name
- **Mobile client** — phone Bluetooth connection state (reported by the ESP32)
- **Laptop / Orin client** — any additional Bluetooth client
- Raw and calibrated accelerometer and gyro values
- Recent ESP32 messages

The four virtual arrow buttons and keyboard arrow keys drive the rover. Hold an
arrow to move; release it to stop.

---

## 8. Label phone and Orin addresses in the sketch (optional)

The ESP32 prints which client type connected when it receives a Bluetooth
connection.  To show `MOBILE` or `LAPTOP` instead of `UNKNOWN`, set the two
constants near the top of `mpu_zero.ino`:

```cpp
const char* MOBILE_BT_ADDRESS  = "PHONE_MAC";   // phone Bluetooth MAC
const char* LAPTOP_BT_ADDRESS  = "ORIN_MAC";    // Orin Bluetooth MAC (if used)
```

Get the phone MAC by connecting the phone to the ESP32 via Bluetooth and
watching the Arduino Serial Monitor — the ESP32 prints the peer address.

Re-upload the sketch after editing.

---

## 9. Test without hardware (dry-run)

On any Linux machine, without an ESP32 connected:

```bash
python rover_server.py --dry-run
```

Open `http://127.0.0.1:8080/` and press the virtual arrow buttons.  Commands
are printed in the terminal instead of being sent to hardware.

API testing:

```bash
# Send a command
curl -X POST http://127.0.0.1:8080/api/command \
  -H 'Content-Type: application/json' \
  --data '{"command":"F5"}'

# Check server status
curl http://127.0.0.1:8080/api/status

# List detected serial ports
curl http://127.0.0.1:8080/api/serial_ports
```

---

## 10. Start automatically at boot

`rover-server.service` is an optional systemd user service. Edit paths if the
folder is not at `~/rover/jetson_orin`, then:

```bash
mkdir -p ~/.config/systemd/user
cp rover-server.service ~/.config/systemd/user/
systemctl --user daemon-reload
systemctl --user enable --now rover-server.service
loginctl enable-linger "$USER"   # keep the service running after logout
```

Check the service:

```bash
systemctl --user status rover-server.service
journalctl --user -u rover-server.service -f
```

The serial port is persisted in `~/.config/rover_bridge/config.json` so no
extra environment variables are needed.

---

## 11. Troubleshooting

| Symptom | Fix |
|---------|-----|
| `Permission denied: '/dev/ttyUSB0'` | `sudo usermod -aG dialout "$USER"` then log out/in |
| `No USB/ACM serial devices found` | Check USB cable (must be data cable, not charge-only); try `ls /dev/ttyUSB* /dev/ttyACM*` |
| Dashboard shows **Serial disconnected** | Reconnect the USB cable and restart the server |
| No telemetry values | Check baud rate is 115200; confirm sketch is uploaded and running |
| Wrong port auto-detected | Pass `--serial-port /dev/ttyACM0` (or whichever device) explicitly |
| Service fails to start | Check `journalctl --user -u rover-server.service` for errors |
| Wheels move unexpectedly | Always test with the wheels off the ground first; keep `S` (stop) ready |
