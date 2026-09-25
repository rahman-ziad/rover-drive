#!/usr/bin/env python3
"""Bluetooth RFCOMM transport – kept for future phone control.

The primary Orin↔ESP32 link is USB-UART (SerialTransport in rover_transport.py).
This module handles optional direct Bluetooth from a phone to the ESP32.
"""

import threading
import time
from collections import deque
from typing import Deque, Dict, Optional

try:
    import bluetooth as _bluetooth
except ImportError:
    _bluetooth = None

try:
    from .rover_transport import TransportError
except ImportError:
    from rover_transport import TransportError

class BluetoothTransport:
    """Line-oriented RFCOMM transport for an ESP32 BluetoothSerial device."""

    def __init__(
        self,
        address: Optional[str] = None,
        name: str = "MPU6050_BOT",
        channel: Optional[int] = None,
    ) -> None:
        self.address = address
        self.name = name
        self.channel = channel

        self._socket = None
        self._socket_lock = threading.RLock()
        self._reader_stop: Optional[threading.Event] = None
        self._reader_thread: Optional[threading.Thread] = None
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

    def _require_pybluez(self) -> None:
        if _bluetooth is None:
            raise TransportError(
                "PyBluez is not installed; install requirements.txt or use --dry-run"
            )

    def _discover_address(self) -> str:
        self._require_pybluez()
        try:
            devices = _bluetooth.discover_devices(lookup_names=True)
        except Exception as exc:
            raise TransportError(f"Bluetooth discovery failed: {exc}") from exc
        wanted = self.name.casefold()
        partial_match: Optional[str] = None
        for device in devices:
            if isinstance(device, (tuple, list)):
                candidate = str(device[0])
                display_name = str(device[1] or "")
            else:
                candidate = str(device)
                display_name = ""
            if display_name.casefold() == wanted:
                return candidate
            if wanted and wanted in display_name.casefold():
                partial_match = candidate
        if partial_match:
            return partial_match
        raise TransportError(f"Bluetooth device {self.name!r} was not found; pass --bluetooth-address")

    def _discover_channel(self, address: str) -> int:
        self._require_pybluez()
        find_service = getattr(_bluetooth, "find_service", None)
        if find_service is not None:
            try:
                services = find_service(address=address)
            except Exception:
                services = []
            for service in services:
                protocol = str(service.get("protocol", "")).upper()
                port = service.get("port")
                if port and (not protocol or protocol == "RFCOMM"):
                    return int(port)
        return 1

    def _connect_locked(self) -> None:
        if self._socket is not None:
            return
        self._require_pybluez()
        address = self.address or self._discover_address()
        channel = self.channel or self._discover_channel(address)
        sock = None
        try:
            sock = _bluetooth.BluetoothSocket(_bluetooth.RFCOMM)
            sock.settimeout(5.0)
            sock.connect((address, channel))
            sock.settimeout(1.0)
        except Exception as exc:
            if sock is not None:
                try:
                    sock.close()
                except Exception:
                    pass
            self._last_error = str(exc)
            raise TransportError(f"Could not connect to {address} on RFCOMM channel {channel}: {exc}") from exc
        self.address = address
        self.channel = channel
        self._last_error = None
        self._socket = sock
        reader_stop = threading.Event()
        self._reader_stop = reader_stop
        self._reader_thread = threading.Thread(
            target=self._read_loop,
            args=(sock, reader_stop),
            name="rover-bluetooth-reader",
            daemon=True,
        )
        self._reader_thread.start()

    def connect(self) -> None:
        """Connect now, or raise a useful error; send_command also retries."""
        with self._socket_lock:
            self._connect_locked()

    def _disconnect_locked(self, sock, error: Optional[str] = None) -> None:
        if self._socket is not sock:
            return
        if self._reader_stop is not None:
            self._reader_stop.set()
        self._socket = None
        self._client["connected"] = False
        self._client["type"] = "NONE"
        self._client["address"] = None
        self._client["updated_at"] = time.time()
        if error:
            self._last_error = error
        try:
            sock.close()
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
            address = line.split(":", 1)[1].strip()
            self._client["address"] = None if address == "NONE" else address

    def _read_loop(self, sock, stop_event: threading.Event) -> None:
        buffer = b""
        error: Optional[str] = None
        while not stop_event.is_set():
            try:
                data = sock.recv(1024)
            except Exception as exc:
                if stop_event.is_set():
                    break
                if "timed out" in str(exc).casefold():
                    continue
                error = str(exc)
                break
            if not data:
                error = "Bluetooth connection closed"
                break
            if isinstance(data, str):
                data = data.encode("utf-8", errors="replace")
            buffer += data
            while b"\n" in buffer:
                raw_line, buffer = buffer.split(b"\n", 1)
                line = raw_line.decode("utf-8", errors="replace").strip()
                if line:
                    with self._socket_lock:
                        self._record_message_locked(line)
        with self._socket_lock:
            self._disconnect_locked(sock, error)

    def send_command(self, command: str) -> None:
        payload = (command + "\n").encode("ascii")
        with self._socket_lock:
            self._connect_locked()
            sock = self._socket
            if sock is None:
                raise TransportError("Bluetooth connection is unavailable")
            try:
                remaining = payload
                while remaining:
                    sent = sock.send(remaining)
                    if sent is None:
                        break
                    if sent <= 0:
                        raise OSError("Bluetooth command was not sent")
                    remaining = remaining[sent:]
                self._last_command = command
            except Exception as exc:
                self._disconnect_locked(sock, str(exc))
                raise TransportError(f"Could not send command: {exc}") from exc

    def status(self) -> Dict[str, object]:
        with self._socket_lock:
            return {
                "transport": "bluetooth-rfcomm",
                "connected": self._socket is not None,
                "address": self.address,
                "name": self.name,
                "channel": self.channel,
                "last_command": self._last_command,
                "last_error": self._last_error,
                "messages": list(self._messages)[-12:],
                "telemetry": {key: list(value) if isinstance(value, list) else value for key, value in self._telemetry.items()},
                "client": dict(self._client),
            }

    def close(self) -> None:
        with self._socket_lock:
            if self._socket is not None:
                self._disconnect_locked(self._socket)

