#!/usr/bin/env python3
"""Install the Tab5 app into a running M5StackLauncher over USB serial -- no SD card.

While the Launcher sits on its boot screen or menu it keeps a serial console open
(src/serial_console.cpp). Its ``flash firmware <name> <size>`` command creates a new
OTA app partition, streams exactly <size> bytes into it with a chunk/ACK handshake,
writes the partition table, selects the new app for boot, records <name> in its app
registry, and reboots into it:

    host -> flash firmware MonsterC5-Tab5 2731296
    dev  <- READY 2731296
    host -> 2048 bytes                       (repeated)
    dev  <- ACK 2048/2731296
    ...
    dev  <- OK flashed, rebooting            (or: ERR <reason>)

Only the app image is sent. That is the same byte range an SD-card install copies out
of the merged image (the app entry's offset onwards), but this path never creates data
partitions -- which this firmware does not use.

Reinstalling under a name that is already installed adds a second copy rather than
replacing it (the Launcher picks a fresh partition label); delete the old one from the
Launcher's app menu first.

Usage:
    tools/launcher_serial_install.py                         # build/M5MonsterC5-Tab5.bin
    tools/launcher_serial_install.py binaries-esp32p4/MonsterC5-Tab5.bin --monitor 30
"""

from __future__ import annotations

import argparse
import os
import re
import struct
import sys
import time
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_IMAGE = REPO_ROOT / "build" / "M5MonsterC5-Tab5.bin"
DEFAULT_NAME = "MonsterC5-Tab5"

CHUNK_SIZE = 2048              # handleFlashCommand() kChunkSize; the device ACKs each chunk
NAME_MAX = 20                  # saveAppNameForLabel() truncates the registry name here
PARTITION_TABLE_OFFSET = 0x8000
APP_SUBTYPES = (0x00, 0x10, 0x20)

LAUNCHER_VERSION_RE = re.compile(r"^Launcher (\S+)$")
READY_RE = re.compile(r"^READY (\d+)$")
ACK_RE = re.compile(r"\bACK (\d+)/(\d+)")
OK_RE = re.compile(r"^OK flashed")


class InstallError(RuntimeError):
    pass


def import_serial():
    """Return the pyserial module, re-running under an ESP-IDF venv if it is missing."""
    try:
        import serial
        return serial
    except ImportError:
        pass
    if not os.environ.get("_LAUNCHER_INSTALL_REEXEC"):
        for python in sorted(Path.home().glob(".espressif/python_env/*/bin/python")):
            if any(python.parent.parent.glob("lib/python*/site-packages/serial")):
                os.environ["_LAUNCHER_INSTALL_REEXEC"] = "1"
                os.execv(str(python), [str(python), str(Path(__file__).resolve()), *sys.argv[1:]])
    raise SystemExit("ERROR: pyserial is not installed (pip install pyserial)")


# ------------------------------------------------------------- image input --

def esp_image_length(data: bytes, offset: int) -> int | None:
    """Exact length of the ESP app image at `offset`.

    Segments, then checksum padding as esp_image_format.c computes it
    ((len + 16) & ~15), then the SHA-256 if appended. Deliberately not the Launcher's
    measureSdEspImage() arithmetic, which overshoots by 16 bytes whenever the segments
    don't end on a 16-byte boundary and so rejects a bare .bin that ends exactly there.
    """
    if offset + 24 > len(data) or data[offset] != 0xE9:
        return None
    segment_count = data[offset + 1]
    if segment_count == 0 or segment_count > 16:
        return None
    cursor = offset + 24
    for _ in range(segment_count):
        if cursor + 8 > len(data):
            return None
        seg_len = struct.unpack_from("<I", data, cursor + 4)[0]
        cursor += 8 + seg_len
        if cursor > len(data):
            return None
    end = (cursor + 16) & ~15
    if data[offset + 23] == 1:  # hash_appended
        end += 32
    return end - offset if end <= len(data) else None


def app_payload(data: bytes) -> tuple[bytes, str]:
    """Return the bytes an SD-card install would copy into the app partition."""
    offset = 0
    where = "bare app image"
    if data[PARTITION_TABLE_OFFSET:PARTITION_TABLE_OFFSET + 3] == b"\xaa\x50\x01":
        for base in range(PARTITION_TABLE_OFFSET, PARTITION_TABLE_OFFSET + 0x1000, 32):
            entry = data[base:base + 32]
            if entry[:2] != b"\xaa\x50":
                break
            if entry[2] == 0x00 and entry[3] in APP_SUBTYPES:
                offset = struct.unpack_from("<I", entry, 4)[0]
                where = f"merged image, app entry @ 0x{offset:X}"
                break
        else:
            raise InstallError("partition table at 0x8000 has no app entry")
        if offset == 0:
            raise InstallError("partition table at 0x8000 has no app entry")
    size = esp_image_length(data, offset)
    if size is None:
        raise InstallError(f"no valid ESP app image at 0x{offset:X}")
    return data[offset:offset + size], where


# ----------------------------------------------------------------- serial --

def open_port(serial, port: str, baud: int):
    ser = serial.Serial()
    ser.port = port
    ser.baudrate = baud
    ser.timeout = 0.05
    # The P4's USB-Serial/JTAG resets the chip when RTS is asserted without DTR. Keep
    # both asserted so opening (and closing) the port never reboots the Launcher.
    ser.dtr = True
    ser.rts = True
    ser.open()
    return ser


class Console:
    def __init__(self, ser):
        self.ser = ser
        self.pending = b""

    def readline(self, deadline: float) -> str | None:
        while True:
            newline = self.pending.find(b"\n")
            if newline >= 0:
                line, self.pending = self.pending[:newline], self.pending[newline + 1:]
                return line.decode("utf-8", "replace").strip()
            if time.monotonic() > deadline:
                return None
            # Read what has arrived (at least one byte) rather than a fixed count, which
            # would sit out the port timeout on every ACK and throttle the transfer.
            self.pending += self.ser.read(self.ser.in_waiting or 1)

    def expect(self, pattern: re.Pattern, timeout: float, what: str, echo: bool = False) -> re.Match:
        deadline = time.monotonic() + timeout
        while True:
            line = self.readline(deadline)
            if line is None:
                raise InstallError(f"timed out after {timeout:.0f}s waiting for {what}")
            if line.startswith("ERR"):
                raise InstallError(f"Launcher replied: {line}")
            match = pattern.search(line)
            if match:
                return match
            if echo and line:
                print(f"  | {line}")

    def command(self, text: str) -> None:
        self.ser.write(text.encode() + b"\n")
        self.ser.flush()


def install(serial, args, payload: bytes) -> None:
    con = Console(open_port(serial, args.port, args.baud))
    try:
        # A bare newline terminates any half-typed line in the console's buffer.
        con.ser.reset_input_buffer()
        con.command("")
        con.command("version")
        version = con.expect(LAUNCHER_VERSION_RE, 3, "the Launcher console").group(1)
        con.command("whoami")
        device = con.readline(time.monotonic() + 3) or "?"
        print(f"  Launcher {version} on '{device}' at {args.port}")
        if "Tab5" not in device and not args.force:
            raise InstallError(f"device reports '{device}', not a Tab5 (use --force to override)")

        size = len(payload)
        con.command(f"flash firmware {args.name} {size}")
        ready = con.expect(READY_RE, 20, "READY", echo=True)
        if int(ready.group(1)) != size:
            raise InstallError(f"device is ready for {ready.group(1)} bytes, not {size}")

        started = time.monotonic()
        written = 0
        next_report = 0
        while written < size:
            chunk = payload[written:written + CHUNK_SIZE]
            con.ser.write(chunk)
            written += len(chunk)
            acked = int(con.expect(ACK_RE, 15, f"ACK {written}/{size}").group(1))
            if acked != written:
                raise InstallError(f"device acknowledged {acked} bytes, host sent {written}")
            percent = written * 100 // size
            if percent >= next_report:
                print(f"\r  streaming {percent:3d}%  {written:,}/{size:,} B", end="", flush=True)
                next_report = percent + 5
        elapsed = time.monotonic() - started
        print(f"\n  streamed in {elapsed:.1f}s ({size / elapsed / 1024:.0f} KiB/s); finalizing")

        try:
            con.expect(OK_RE, 60, "OK flashed", echo=True)
            print("  OK -- installed and selected for boot; the Tab5 is rebooting into it")
        except serial.SerialException:
            # Seen on a Tab5: the USB device drops for the reboot before the "OK flashed"
            # line reaches the host. handleFlashCommand() only reboots on its success path
            # (a failure prints ERR and stays up), so once every byte has been ACKed a
            # disconnect here means the install completed.
            print("  OK -- the Launcher rebooted into the app (every byte was acknowledged; "
                  "its OK line was lost to the reboot)")
    finally:
        con.ser.close()


def monitor(serial, port: str, baud: int, seconds: float) -> None:
    """Print device output for `seconds`, riding out the re-enumeration after reboot."""
    print(f"\n--- monitoring {port} for {seconds:.0f}s (early boot lines may be lost) ---")
    deadline = time.monotonic() + seconds
    ser = None
    while time.monotonic() < deadline:
        if ser is None:
            try:
                ser = open_port(serial, port, baud)
                print("--- [port open] ---")
            except (OSError, serial.SerialException):
                time.sleep(0.2)
                continue
        try:
            data = ser.read(4096)
        except (OSError, serial.SerialException):
            ser.close()
            ser = None
            print("\n--- [port dropped] ---")
            continue
        if data:
            sys.stdout.write(data.decode("utf-8", "replace"))
            sys.stdout.flush()
    if ser is not None:
        ser.close()
    print("\n--- monitor done ---")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("image", nargs="?", type=Path, default=DEFAULT_IMAGE,
                    help="bare app .bin or merged Launcher image (default: build/M5MonsterC5-Tab5.bin)")
    ap.add_argument("--port", default="/dev/ttyACM0")
    ap.add_argument("--baud", type=int, default=115200, help="ignored by USB CDC, kept for UART bridges")
    ap.add_argument("--name", default=DEFAULT_NAME,
                    help=f"name shown in the Launcher (max {NAME_MAX} chars, no spaces)")
    ap.add_argument("--monitor", type=float, default=0, metavar="SECONDS",
                    help="after installing, print the device's output for this long")
    ap.add_argument("--force", action="store_true", help="install even if the device is not a Tab5")
    args = ap.parse_args()

    if not args.name or any(c.isspace() for c in args.name):
        print("ERROR: --name must be non-empty and contain no spaces", file=sys.stderr)
        return 2
    if len(args.name) > NAME_MAX:
        print(f"warning: the Launcher truncates names to {NAME_MAX} chars: '{args.name[:NAME_MAX]}'")

    serial = import_serial()
    try:
        if not args.image.exists():
            raise InstallError(f"no such file: {args.image}")
        payload, where = app_payload(args.image.read_bytes())
        print(f"Installing {args.image.name} ({where}, {len(payload):,} B) as '{args.name}'")
        install(serial, args, payload)
    except (InstallError, serial.SerialException, OSError) as exc:
        print(f"\nERROR: {exc}", file=sys.stderr)
        return 1
    if args.monitor > 0:
        monitor(serial, args.port, args.baud, args.monitor)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
