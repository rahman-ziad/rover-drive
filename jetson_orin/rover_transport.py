#!/usr/bin/env python3
"""Rover transport layer.

USB-UART (SerialTransport) is the primary connection to the ESP32.
BluetoothTransport lives in bluetooth_transport.py for future phone control.
"""

import threading
import time
from collections import deque
from typing import Deque, Dict, Optional

import serial

try:
    import bluetooth as _bluetooth
except ImportError:
    _bluetooth = None


class TransportError(RuntimeError):
    """Raised when a command cannot be delivered to the rover."""


class LoggingTransport:
    """Dry-run transport for testing the web UI without a connected rover."""

    def __init__(self) -> None:
        self._commands: Deque[str] = deque(maxlen=50)

    def connect(self) -> None:
        pass

    def send_command(self, command: str) -> None:
        self._commands.append(command)
        print("DRY RUN:", command, flush=True)

    def status(self) -> Dict[str, object]:
        return {
            "transport": "dry-run",
            "connected": True,
            "port": None,
            "commands": list(self._commands),
            "messages": [],
            "telemetry": {
                "accel_raw": None,
                "accel_calibrated": None,
                "gyro_raw": None,
                "gyro_calibrated": None,
                "updated_at": None,
            },
            "client": {"connected": False, "type": "NONE", "address": None, "updated_at": None},
        }

    def close(self) -> None:
        pass


class SerialTransport:
    """USB-UART (pyserial) transport for the ESP32.

    Mirrors the public API of BluetoothTransport so the server stays unchanged.
    Auto-detects the first /dev/ttyUSB* or /dev/ttyACM* device if port is None.

    KEY: we open the port with dsrdtr=False so the DTR line is NOT toggled,
    which prevents the ESP32 from resetting every time the port is opened.
    """

    def __init__(self, port: Optional[str] = None, baudrate: int = 115200) -> None:
        self.port = port
        self.baudrate = baudrate
        self._serial: Optional[serial.Serial] = None
        self._lock = threading.RLock()
        self._reader_thread: Optional[threading.Thread] = None
        self._reader_stop: Optional[threading.Event] = None
        self._messages: Deque[str] = deque(maxlen=80)
        self._last_error: Optional[str] = None
        self._last_command: Optional[str] = None
        self._telemetry: Dict[str, object] = {
            "accel_raw": None,
            "accel_calibrated": None,
            "gyro_raw": None,
            "gyro_calibrated": None,
            "updated_at": None,
        }
        self._client: Dict[str, object] = {
            "connected": False,
            "type": "NONE",
            "address": None,
            "updated_at": None,
        }

    def _discover_port(self) -> str:
        """Return the first /dev/ttyUSB* or /dev/ttyACM* device found."""
        import glob
        candidates = sorted(glob.glob("/dev/ttyUSB*") + glob.glob("/dev/ttyACM*"))
        if not candidates:
            raise TransportError("No USB/ACM serial devices found. Is the ESP32 plugged in?")
        return candidates[0]

    def connect(self) -> None:
        """Open the serial port and start the background reader thread."""
        with self._lock:
            if self._serial is not None:
                return
            port = self.port or self._discover_port()
            try:
                # dsrdtr=False  → don't assert DTR, so the ESP32 won't reset on open
                # rtscts=False  → no hardware flow control
                ser = serial.Serial(
                    port,
                    self.baudrate,
                    timeout=1,
                    write_timeout=2,
                    dsrdtr=False,
                    rtscts=False,
                )
            except Exception as exc:
                self._last_error = str(exc)
                raise TransportError(f"Could not open serial port {port}: {exc}") from exc

            self.port = port          # remember the actual port used
            self._serial = ser
            self._last_error = None
            self._reader_stop = threading.Event()
            self._reader_thread = threading.Thread(
                target=self._read_loop,
                args=(ser, self._reader_stop),
                name="rover-serial-reader",
                daemon=True,
            )
            self._reader_thread.start()

    def reconnect(self, port: Optional[str] = None) -> None:
        """Close the current connection (if any) then open a new one.

        We MUST join the old reader thread before re-opening the port,
        otherwise the OS still considers the device file in use and the
        new serial.Serial() call will fail or get stale data.
        """
        old_thread: Optional[threading.Thread] = None
        with self._lock:
            if self._serial is not None:
                old_thread = self._reader_thread
                self._disconnect_locked(self._serial, "reconnect requested")
            if port:
                self.port = port
            else:
                # clear remembered port so auto-detect runs again
                if port == "":
                    self.port = None

        # Wait up to 3 s for the reader thread to exit
        if old_thread is not None and old_thread.is_alive():
            old_thread.join(timeout=3.0)

        # Small OS-level grace period so the device node is fully released
        time.sleep(0.15)

        self.connect()


    def _read_loop(self, ser: serial.Serial, stop_event: threading.Event) -> None:
        """Read lines from the serial port; retry on transient errors."""
        error: Optional[str] = None
        consecutive_errors = 0

        while not stop_event.is_set():
            try:
                line_bytes = ser.readline()
                consecutive_errors = 0   # reset on any successful call
            except serial.SerialException as exc:
                if stop_event.is_set():
                    break
                consecutive_errors += 1
                if consecutive_errors >= 8:
                    # Too many consecutive errors → give up
                    error = str(exc)
                    break
                # Transient error (e.g. ESP32 still booting) — wait and retry
                time.sleep(0.25)
                continue
            except Exception as exc:
                if stop_event.is_set():
                    break
                error = str(exc)
                break

            if not line_bytes:
                # Timeout (no data within 1 s) — normal, just loop
                continue

            line = line_bytes.decode("utf-8", errors="replace").strip()
            if line:
                with self._lock:
                    self._record_message_locked(line)

        with self._lock:
            self._disconnect_locked(ser, error)

    def _disconnect_locked(self, ser: serial.Serial, error: Optional[str] = None) -> None:
        if self._serial is not ser:
            return
        if self._reader_stop is not None:
            self._reader_stop.set()
        self._serial = None
        self._client["connected"] = False
        self._client["type"] = "NONE"
        self._client["address"] = None
        self._client["updated_at"] = time.time()
        if error:
            self._last_error = error
        try:
            ser.close()
        except Exception:
            pass

    def _record_message_locked(self, line: str) -> None:
        self._messages.append(line)
        telemetry_prefixes = (
            ("ACC RAW:", "accel_raw"),
            ("ACC CAL:", "accel_calibrated"),
            ("GYRO RAW:", "gyro_raw"),
            ("GYRO CAL:", "gyro_calibrated"),
        )
        for prefix, key in telemetry_prefixes:
            if line.startswith(prefix):
                try:
                    values = [float(v) for v in line[len(prefix):].split()]
                except ValueError:
                    return
                if len(values) == 3:
                    self._telemetry[key] = values
                    self._telemetry["updated_at"] = time.time()
                return

        # Phone Bluetooth client status forwarded via USB-UART
        if line == "BT STATUS: CONNECTED":
            self._client["connected"] = True
            self._client["updated_at"] = time.time()
        elif line == "BT STATUS: DISCONNECTED":
            self._client["connected"] = False
            self._client["type"] = "NONE"
            self._client["address"] = None
            self._client["updated_at"] = time.time()
        elif line.startswith("BT CLIENT TYPE:"):
            self._client["type"] = line.split(":", 1)[1].strip()
        elif line.startswith("BT CLIENT ADDRESS:"):
            addr = line.split(":", 1)[1].strip()
            self._client["address"] = None if addr == "NONE" else addr

    def send_command(self, command: str) -> None:
        payload = (command + "\n").encode("ascii")
        with self._lock:
            if self._serial is None:
                raise TransportError("Serial connection is unavailable")
            try:
                self._serial.write(payload)
                self._last_command = command
            except Exception as exc:
                self._disconnect_locked(self._serial, str(exc))
                raise TransportError(f"Could not send command: {exc}") from exc

    def status(self) -> Dict[str, object]:
        with self._lock:
            return {
                "transport": "serial",
                "connected": self._serial is not None,
                "port": self.port,
                "last_command": self._last_command,
                "last_error": self._last_error,
                "messages": list(self._messages)[-12:],
                "telemetry": {
                    k: list(v) if isinstance(v, list) else v
                    for k, v in self._telemetry.items()
                },
                "client": dict(self._client),
            }

    def close(self) -> None:
        with self._lock:
            if self._serial is not None:
                self._disconnect_locked(self._serial)


# BluetoothTransport kept in its own module for future phone control.
try:
    from .bluetooth_transport import BluetoothTransport
except ImportError:
    from bluetooth_transport import BluetoothTransport
