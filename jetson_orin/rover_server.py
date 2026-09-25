#!/usr/bin/env python3
"""Wi-Fi control server – USB-UART bridge for the rover.

Browser/laptop → HTTP over Wi-Fi → this server → USB-UART (pyserial) → ESP32.
The server accepts the same newline-delimited commands as the ESP32 sketch.
Bluetooth args are kept for optional future phone control.
"""

import argparse
import glob
import json
import os
import re
import signal
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import List, Optional
from urllib.parse import urlsplit

try:
    from .rover_transport import LoggingTransport, SerialTransport, TransportError
    try:
        from .bluetooth_transport import BluetoothTransport
    except ImportError:
        BluetoothTransport = None
except ImportError:
    from rover_transport import LoggingTransport, SerialTransport, TransportError
    try:
        from bluetooth_transport import BluetoothTransport
    except ImportError:
        BluetoothTransport = None


ROOT = Path(__file__).resolve().parent
INDEX_FILE = ROOT / "web" / "index.html"
CONFIG_FILE = Path.home() / ".config" / "rover_bridge" / "config.json"
MAX_BODY_BYTES = 4096
MAX_FORWARD_SECONDS = 3600.0
NUMBER = r"(?:\d+(?:\.\d*)?|\.\d+)"
COMMAND_PATTERN = re.compile(
    r"^(?:S|[BLR]|F(?:{0})?|RL{0}|RR{0})$".format(NUMBER)
)


# ---------------------------------------------------------------------------
# Config persistence (stores the last-used serial port)
# ---------------------------------------------------------------------------

def load_config() -> dict:
    try:
        return json.loads(CONFIG_FILE.read_text())
    except Exception:
        return {}


def save_config(data: dict) -> None:
    try:
        CONFIG_FILE.parent.mkdir(parents=True, exist_ok=True)
        CONFIG_FILE.write_text(json.dumps(data, indent=2))
    except Exception as exc:
        print(f"[config] Could not save config: {exc}", file=sys.stderr)


# ---------------------------------------------------------------------------
# Command validation
# ---------------------------------------------------------------------------

def normalize_command(command: str) -> str:
    """Validate and normalise one command before it reaches the ESP32."""
    if not isinstance(command, str):
        raise ValueError("command must be a string")
    normalized = command.strip().upper()
    if not normalized:
        raise ValueError("command is empty")
    if "\n" in normalized or "\r" in normalized:
        raise ValueError("only one command is allowed per line")
    if not COMMAND_PATTERN.fullmatch(normalized):
        raise ValueError("unsupported command: {}".format(normalized))
    if normalized.startswith("F") and len(normalized) > 1:
        seconds = float(normalized[1:])
        if seconds <= 0 or seconds > MAX_FORWARD_SECONDS:
            raise ValueError("F duration must be between 0 and 3600 seconds")
    if normalized.startswith("RL") or normalized.startswith("RR"):
        degrees = float(normalized[2:])
        if degrees <= 0:
            raise ValueError("rotation angle must be positive")
    return normalized


def parse_commands(body: bytes, content_type: str) -> List[str]:
    """Parse JSON or plain-text command requests."""
    try:
        text = body.decode("utf-8")
    except UnicodeDecodeError as exc:
        raise ValueError("request body must be UTF-8") from exc

    media_type = content_type.split(";", 1)[0].strip().lower()
    if media_type == "application/json":
        try:
            payload = json.loads(text)
        except json.JSONDecodeError as exc:
            raise ValueError("request body is not valid JSON") from exc
        if isinstance(payload, dict):
            text = payload.get("command", "")
        elif isinstance(payload, str):
            text = payload
        else:
            raise ValueError("JSON body must contain a command string")

    if not isinstance(text, str):
        raise ValueError("command must be a string")

    lines = [line for line in text.splitlines() if line.strip()]
    if not lines:
        raise ValueError("request contains no command")
    return [normalize_command(line) for line in lines]


# ---------------------------------------------------------------------------
# HTTP handler
# ---------------------------------------------------------------------------

class RoverRequestHandler(BaseHTTPRequestHandler):
    """HTTP API and browser UI handler."""

    server_version = "RoverServer/1.0"

    def _send_json(self, status: int, payload: object) -> None:
        data = json.dumps(payload).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("Access-Control-Allow-Origin", "*")
        self.end_headers()
        self.wfile.write(data)

    def _send_bytes(self, status: int, data: bytes, content_type: str) -> None:
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(data)

    def _read_body(self) -> bytes:
        value = self.headers.get("Content-Length")
        if value is None:
            raise ValueError("Content-Length is required")
        try:
            length = int(value)
        except ValueError as exc:
            raise ValueError("Content-Length is invalid") from exc
        if length < 0 or length > MAX_BODY_BYTES:
            raise ValueError("request body is too large")
        return self.rfile.read(length)

    def do_OPTIONS(self) -> None:
        self.send_response(204)
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "Content-Type")
        self.end_headers()

    def do_GET(self) -> None:
        path = urlsplit(self.path).path

        if path == "/":
            try:
                data = INDEX_FILE.read_bytes()
            except OSError as exc:
                self._send_json(500, {"error": "web UI is unavailable", "detail": str(exc)})
                return
            self._send_bytes(200, data, "text/html; charset=utf-8")
            return

        if path == "/api/status":
            self._send_json(200, self.server.transport.status())
            return

        if path == "/api/serial_ports":
            ports = sorted(glob.glob("/dev/ttyUSB*") + glob.glob("/dev/ttyACM*"))
            self._send_json(200, {"ports": ports})
            return

        if path == "/healthz":
            self._send_json(200, {"ok": True})
            return

        self._send_json(404, {"error": "not found"})

    def do_POST(self) -> None:
        path = urlsplit(self.path).path

        # ── Serial port (re)connect ──────────────────────────────────────────
        if path == "/api/connect":
            # Read body defensively — Content-Length may or may not be present
            payload: dict = {}
            try:
                cl = self.headers.get("Content-Length")
                if cl:
                    raw = self.rfile.read(int(cl))
                    if raw.strip():
                        payload = json.loads(raw)
            except Exception:
                pass   # empty or malformed body → use defaults

            port = payload.get("port") or None   # "" or None → auto-detect
            baudrate = int(payload.get("baudrate") or 115200)

            transport = self.server.transport
            if not hasattr(transport, "reconnect"):
                self._send_json(400, {"error": "current transport does not support reconnect"})
                return

            # Update baudrate if changed
            if hasattr(transport, "baudrate"):
                transport.baudrate = baudrate

            try:
                transport.reconnect(port)
                # Persist
                cfg = load_config()
                if transport.port:
                    cfg["serial_port"] = transport.port
                save_config(cfg)
                self._send_json(200, transport.status())
            except TransportError as exc:
                self._send_json(503, {"error": str(exc), "transport": transport.status()})
            return


        # ── Rover commands ───────────────────────────────────────────────────
        if path not in ("/api/command", "/command"):
            self._send_json(404, {"error": "not found"})
            return

        try:
            commands = parse_commands(
                self._read_body(), self.headers.get("Content-Type", "text/plain")
            )
        except ValueError as exc:
            self._send_json(400, {"error": str(exc)})
            return

        try:
            for command in commands:
                self.server.transport.send_command(command)
        except TransportError as exc:
            self._send_json(
                503,
                {"error": str(exc), "accepted": [], "transport": self.server.transport.status()},
            )
            return

        self._send_json(
            200,
            {"accepted": commands, "transport": self.server.transport.status()},
        )

    def log_message(self, format: str, *args) -> None:
        print("[http] " + (format % args), flush=True)


# ---------------------------------------------------------------------------
# HTTP server
# ---------------------------------------------------------------------------

class RoverHTTPServer(ThreadingHTTPServer):
    """Threaded HTTP server carrying the selected rover transport."""

    allow_reuse_address = True
    daemon_threads = True

    def __init__(self, address, handler, transport) -> None:
        super().__init__(address, handler)
        self.transport = transport


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def make_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Run the Jetson rover Wi-Fi bridge")
    parser.add_argument(
        "--host",
        default="0.0.0.0",
        help="interface to listen on (default: all interfaces)",
    )
    parser.add_argument("--port", type=int, default=8080, help="HTTP port (default: 8080)")
    parser.add_argument(
        "--serial-port",
        default=None,
        help="USB/ACM serial device (e.g. /dev/ttyUSB0); auto-detect if omitted",
    )
    parser.add_argument(
        "--bluetooth-address",
        default=os.environ.get("ROVER_BT_ADDRESS"),
        help="(future) ESP32 Bluetooth MAC; can also be set with ROVER_BT_ADDRESS",
    )
    parser.add_argument(
        "--bluetooth-name",
        default=os.environ.get("ROVER_BT_NAME", "MPU6050_BOT"),
        help="(future) Bluetooth device name for discovery (default: MPU6050_BOT)",
    )
    parser.add_argument(
        "--bluetooth-channel",
        type=int,
        default=None,
        help="(future) RFCOMM channel (default: service discovery, then 1)",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="log commands to stdout instead of sending to ESP32",
    )
    return parser


def main() -> int:
    args = make_parser().parse_args()
    if not 1 <= args.port <= 65535:
        print("Port must be between 1 and 65535", file=sys.stderr)
        return 2

    cfg = load_config()

    if args.dry_run:
        transport = LoggingTransport()
        print("DRY RUN mode — no hardware required", flush=True)
    else:
        # Determine which serial port to use
        serial_port: Optional[str] = args.serial_port or cfg.get("serial_port")

        transport = SerialTransport(port=serial_port)
        try:
            transport.connect()
            print(f"Serial rover connected on {transport.port}", flush=True)
            # Persist the port for next startup
            cfg["serial_port"] = transport.port
            save_config(cfg)
        except TransportError as exc:
            print(
                f"Serial not connected yet: {exc}\n"
                "The web UI will stay available. Reconnect the USB cable and restart.",
                file=sys.stderr,
                flush=True,
            )

    try:
        server = RoverHTTPServer((args.host, args.port), RoverRequestHandler, transport)
    except OSError as exc:
        print(f"Could not start HTTP server: {exc}", file=sys.stderr)
        transport.close()
        return 1

    print(
        f"Rover server listening on http://{args.host}:{args.port}/",
        flush=True,
    )
    print("Open the Orin's IP address in your browser from any device on the same Wi-Fi.", flush=True)

    def stop_on_signal(signum, frame) -> None:
        raise KeyboardInterrupt

    signal.signal(signal.SIGTERM, stop_on_signal)

    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nStopping rover server", flush=True)
    finally:
        server.server_close()
        try:
            if transport.status().get("connected"):
                transport.send_command("S")
        except TransportError:
            pass
        transport.close()

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
