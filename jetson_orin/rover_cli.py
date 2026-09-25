#!/usr/bin/env python3
"""Send ESP32 rover commands directly from an Orin terminal over USB-UART.

Usage:
    python rover_cli.py                        # auto-detect serial port
    python rover_cli.py --serial-port /dev/ttyUSB0
    python rover_cli.py --dry-run              # no hardware needed

Type commands like F5, S, RL90; press Ctrl-D to exit.
"""

import argparse
import sys

try:
    from .rover_server import normalize_command
    from .rover_transport import LoggingTransport, SerialTransport, TransportError
except ImportError:
    from rover_server import normalize_command
    from rover_transport import LoggingTransport, SerialTransport, TransportError


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Send commands to the ESP32 over USB-UART"
    )
    parser.add_argument(
        "--serial-port",
        default=None,
        help="serial device, e.g. /dev/ttyUSB0 (auto-detect if omitted)",
    )
    parser.add_argument(
        "--baudrate",
        type=int,
        default=115200,
        help="baud rate (default: 115200)",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="print commands instead of sending to hardware",
    )
    args = parser.parse_args()

    if args.dry_run:
        transport = LoggingTransport()
        print("DRY RUN — commands will be printed, not sent.")
    else:
        transport = SerialTransport(port=args.serial_port, baudrate=args.baudrate)
        try:
            transport.connect()
            print(f"Connected on {transport.port}. Type commands (e.g. F5, S, RL90); Ctrl-D exits.")
        except TransportError as exc:
            print(f"Serial error: {exc}", file=sys.stderr)
            return 1

    try:
        for line in sys.stdin:
            if not line.strip():
                continue
            try:
                command = normalize_command(line)
                transport.send_command(command)
                print("sent:", command)
            except (ValueError, TransportError) as exc:
                print("error:", exc, file=sys.stderr)
    finally:
        try:
            transport.send_command("S")   # safety stop on exit
        except TransportError:
            pass
        transport.close()

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
