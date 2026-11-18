#!/usr/bin/env python3
"""Manage /sdcard on the Tiny386 handheld over USB-Serial/JTAG.

The device exposes a tiny file console while the boot menu is visible
(sdcli.c).  It is stopped automatically before the DOS emulator starts.

Examples:
    ./sdctl.py ls /
    ./sdctl.py cat /tiny386.ini
    ./sdctl.py get /menu.ini menu.ini
    ./sdctl.py put menu.ini /menu.ini
    ./sdctl.py rm /menu.ini.bak
    ./sdctl.py reboot

Requires pyserial.  Run it while the game menu is on screen; opening the
port may reset the board, in which case wait for the menu and retry.
"""

import argparse
import glob
import re
import sys
import time
from pathlib import Path

import serial

ANSI = re.compile(rb"\x1b\[[0-9;]*m")
PROMPT = rb"sd> "


def clean(data: bytes) -> str:
    data = ANSI.sub(b"", data)
    text = data.decode("utf-8", "replace")
    return text.replace("\r\n", "\n").replace("\r", "\n")


def find_port() -> str:
    for pattern in ("/dev/ttyACM*", "/dev/ttyUSB*", "/dev/cu.usbmodem*"):
        ports = sorted(glob.glob(pattern))
        if ports:
            return ports[0]
    raise SystemExit("no serial port found; pass --port")


class Device:
    def __init__(self, port: str):
        self.ser = serial.Serial(
            port=port,
            baudrate=115200,
            timeout=0.2,
            write_timeout=30,
            dsrdtr=False,
            rtscts=False,
        )
        self.buf = bytearray()

    def close(self):
        self.ser.close()

    def _fill(self, timeout=0.2):
        data = self.ser.read(4096)
        if data:
            self.buf.extend(data)
            return True
        time.sleep(timeout)
        return False

    def _read_until(self, pattern, timeout):
        rx = re.compile(pattern, re.MULTILINE)
        deadline = time.time() + timeout
        while time.time() < deadline:
            # Search an immutable snapshot: a match over the live bytearray
            # would shift when the consumed prefix is deleted below.
            snapshot = bytes(self.buf)
            match = rx.search(snapshot)
            if match:
                data = snapshot[: match.end()]
                del self.buf[: match.end()]
                return match, data
            self._fill()
        return None, bytes(self.buf)

    def _read_exact(self, size, timeout):
        deadline = time.time() + timeout
        while len(self.buf) < size and time.time() < deadline:
            self._fill()
        data = bytes(self.buf[:size])
        del self.buf[:size]
        return data

    def wait_prompt(self, timeout=15):
        deadline = time.time() + timeout
        while time.time() < deadline:
            self.ser.write(b"\n")
            match, _ = self._read_until(PROMPT, 1.0)
            if match:
                return
        raise SystemExit(
            "no 'sd>' prompt. Is the device showing the game menu? "
            "If it is already in DOS, reboot it first."
        )

    def command(self, line, timeout=15):
        self.ser.write((line + "\n").encode())
        match, data = self._read_until(PROMPT, timeout)
        text = clean(data)
        if "sd>" in text:
            text = text.rsplit("sd>", 1)[0]
        lines = text.split("\n")
        if lines and lines[0].strip() == line:
            lines = lines[1:]
        return "\n".join(lines).rstrip()

    def get(self, remote, local, timeout=120):
        self.ser.write(("get " + remote + "\n").encode())
        match, _ = self._read_until(rb"^OK (\d+)\r?\n", 15)
        if not match:
            raise SystemExit("get: no OK response")
        size = int(match.group(1))
        blob = self._read_exact(size, timeout)
        match, _ = self._read_until(PROMPT, 15)
        if len(blob) != size:
            raise SystemExit(f"get: short read {len(blob)}/{size}")
        Path(local).write_bytes(blob)
        print(f"saved {remote} -> {local} ({size} bytes)")

    def put(self, local, remote, timeout=120):
        blob = Path(local).read_bytes()
        self.ser.write((f"put {remote} {len(blob)}\n").encode())
        match, _ = self._read_until(rb"^READY\r?$", 15)
        if not match:
            raise SystemExit("put: device did not answer READY")
        self.ser.write(blob)
        match, data = self._read_until(PROMPT, timeout)
        text = clean(data)
        if "OK" not in text:
            raise SystemExit("put failed:\n" + text.strip())
        print(f"sent {local} -> {remote} ({len(blob)} bytes)")


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--port", default="auto", help="serial port (default: autodetect)")
    sub = parser.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("ls")
    p.add_argument("path", nargs="?", default="/")

    p = sub.add_parser("cat")
    p.add_argument("path")

    p = sub.add_parser("get")
    p.add_argument("remote")
    p.add_argument("local")

    p = sub.add_parser("put")
    p.add_argument("local")
    p.add_argument("remote")

    p = sub.add_parser("rm")
    p.add_argument("path")

    sub.add_parser("free")
    sub.add_parser("reboot")

    args = parser.parse_args()
    port = find_port() if args.port == "auto" else args.port
    dev = Device(port)
    try:
        dev.wait_prompt()
        if args.cmd == "ls":
            print(dev.command("ls " + args.path))
        elif args.cmd == "cat":
            print(dev.command("cat " + args.path))
        elif args.cmd == "rm":
            print(dev.command("rm " + args.path))
        elif args.cmd == "free":
            print(dev.command("free"))
        elif args.cmd == "get":
            dev.get(args.remote, args.local)
        elif args.cmd == "put":
            dev.put(args.local, args.remote)
        elif args.cmd == "reboot":
            dev.ser.write(b"reboot\n")
            time.sleep(0.5)
            print("reboot requested")
    finally:
        dev.close()


if __name__ == "__main__":
    main()
