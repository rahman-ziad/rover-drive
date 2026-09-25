# Jetson Orin rover bridge

For a full wiring, upload, and startup procedure see `GETTING_STARTED.md`.

This folder runs on the Jetson Orin. The control path is:

```text
browser / laptop  ──── Wi-Fi / HTTP ────►  Orin server  ──── USB-UART ────►  ESP32
```

The Orin connects to the ESP32 with a **USB cable** (pyserial, `/dev/ttyUSB*` or
`/dev/ttyACM*`).  Bluetooth is still supported by the ESP32 for direct **phone
control** but is not used by the Orin server.

## ESP32 commands

Commands are newline-delimited. The server validates and forwards them:

| Command | Action |
| --- | --- |
| `F` | Forward while held; send `S` to stop |
| `F5` | Forward for 5 seconds |
| `F0.8` | Forward for 0.8 seconds |
| `B` | Backward while held; send `S` to stop |
| `L` | Rotate left while held; send `S` to stop |
| `R` | Rotate right while held; send `S` to stop |
| `S` | Stop immediately |
| `RL90` | Rotate left 90 degrees (gyro-assisted) |
| `RR90` | Rotate right 90 degrees (gyro-assisted) |

`F<number>` accepts a positive duration up to 3600 seconds. Bare `F`, `B`, `L`,
and `R` are hold commands — the webpage repeats them every 250 ms and sends `S`
on release. The ESP32 stops a hold command after one second without a refresh as
a safety timeout. Every command must end with `\n`; the Python transport adds it
automatically.

## Install on the Orin

Install Python and serial support:

```bash
sudo apt update
sudo apt install -y python3-pip python3-venv
python3 -m venv .venv
. .venv/bin/activate
pip install -r requirements.txt
```

Add your user to the `dialout` group so Python can open the serial port:

```bash
sudo usermod -aG dialout "$USER"
# Log out and back in (or reboot) for this to take effect.
```

## Start the Wi-Fi server

Plug the ESP32 into the Orin with a USB cable, then from this folder: 

```bash
. .venv/bin/activate
python rover_server.py
```

The server auto-detects the first `/dev/ttyUSB*` or `/dev/ttyACM*` device and
saves the chosen port to `~/.config/rover_bridge/config.json` so it reconnects
automatically after a reboot.  You can also specify the port explicitly:

```bash
python rover_server.py --serial-port /dev/ttyUSB0
```

The server listens on `0.0.0.0:8080`. Find the Orin's Wi-Fi address:

```bash
hostname -I
```

Then open this URL from any device on the same network:

```text
http://ORIN_IP:8080/
```

The page maps keyboard arrow keys and four virtual buttons to held movement.
Releasing a key or button sends `S`. The dashboard shows the serial link state,
phone Bluetooth client state, accelerometer values, and gyro values.

## Test without hardware

Use dry-run mode to verify the web page and HTTP API on any Linux machine:

```bash
python rover_server.py --dry-run
```

Commands are printed to the terminal instead of being sent to the ESP32.  You
can also test the API directly:

```bash
# Plain-text endpoint
curl -X POST http://127.0.0.1:8080/command \
  -H 'Content-Type: text/plain' \
  --data-binary $'F5\n'

# JSON endpoint (used by the web page)
curl -X POST http://127.0.0.1:8080/api/command \
  -H 'Content-Type: application/json' \
  --data '{"command":"S"}'

# List detected serial ports
curl http://127.0.0.1:8080/api/serial_ports
```

## Start automatically at boot

`rover-server.service` is an optional systemd user service. Edit the paths in
the unit file if needed, then:

```bash
mkdir -p ~/.config/systemd/user
cp rover-server.service ~/.config/systemd/user/
systemctl --user daemon-reload
systemctl --user enable --now rover-server.service
loginctl enable-linger "$USER"
```

Check the service:

```bash
systemctl --user status rover-server.service
journalctl --user -u rover-server.service -f
```

The serial port is auto-detected and persisted, so no environment variable is
needed for the service.

## Network note

The server has no authentication. Anyone who can reach port 8080 on the Orin
can send rover commands — use it only on a trusted local network or add firewall
rules before exposing the port.
