#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""PortLens flash station — plug boards in, they get flashed.

Watches for USB-serial adapters (CH340 on the Olimex ESP32-PoE-ISO). Every
newly plugged board is flashed automatically, several in parallel, and each
result is appended to flash_log.csv with the board's Ethernet MAC (the one
shown on the PortLens display and seen by the switch).

Unplug a board and plug in the next one; re-plugging the same board flashes
it again. Ctrl+C to quit.

Requires esptool + pyserial. Easiest is the ESP-IDF Python env:
    . ~/esp/esp-idf/export.sh && python tools/flash_station.py
or: pip install esptool pyserial
"""

import argparse
import csv
import datetime
import os
import re
import subprocess
import sys
import threading
import time

try:
    import serial.tools.list_ports
except ImportError:
    sys.exit("pyserial not found — run inside the ESP-IDF env or `pip install esptool pyserial`")

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Offsets from build/flasher_args.json (bootloader / partition table / app)
IMAGES = [
    (0x1000, "bootloader.bin"),
    (0x8000, "partition-table.bin"),
    (0x10000, "portlens.bin"),
]
BUILD_PATHS = {
    "bootloader.bin": "build/bootloader/bootloader.bin",
    "partition-table.bin": "build/partition_table/partition-table.bin",
    "portlens.bin": "build/portlens.bin",
}

# CH340/CH9102 (WCH) and the common CP210x / FTDI bridges
USB_VIDS = {0x1A86, 0x10C4, 0x0403}

GREEN, RED, YELLOW, DIM, RESET = "\033[32m", "\033[31m", "\033[33m", "\033[2m", "\033[0m"

print_lock = threading.Lock()


def log(msg, color=""):
    stamp = datetime.datetime.now().strftime("%H:%M:%S")
    with print_lock:
        print(f"{DIM}{stamp}{RESET} {color}{msg}{RESET}", flush=True)


def resolve_images(source):
    """Return [(offset, path)] for the chosen firmware source."""
    if source == "auto":
        source = "build" if os.path.isfile(os.path.join(REPO, BUILD_PATHS["portlens.bin"])) else "docs"
    if source == "build":
        paths = [(off, os.path.join(REPO, BUILD_PATHS[name])) for off, name in IMAGES]
    else:
        paths = [(off, os.path.join(REPO, "docs", "firmware", name)) for off, name in IMAGES]
    missing = [p for _, p in paths if not os.path.isfile(p)]
    if missing:
        sys.exit("missing firmware image(s):\n  " + "\n  ".join(missing))
    return source, paths


def eth_mac(base_mac):
    """ESP32 with 4 universal MACs: Ethernet MAC = base (WiFi STA) MAC + 3."""
    n = (int(base_mac.replace(":", ""), 16) + 3) & 0xFFFFFFFFFFFF
    return ":".join(f"{(n >> s) & 0xFF:02X}" for s in range(40, -8, -8))


def candidate_ports():
    ports = {}
    for p in serial.tools.list_ports.comports():
        if p.vid in USB_VIDS or re.search(r"wchusbserial|usbserial|SLAB_USBtoUART|ttyUSB", p.device):
            # macOS exposes both /dev/tty.* and /dev/cu.*; use cu.*
            if p.device.startswith("/dev/tty.") and os.path.exists(p.device.replace("/dev/tty.", "/dev/cu.")):
                continue
            ports[p.device] = p
    return ports


class Station:
    def __init__(self, images, baud, erase, log_path):
        self.images = images
        self.baud = baud
        self.erase = erase
        self.log_path = log_path
        self.busy = set()      # ports currently flashing
        self.handled = set()   # ports finished; cleared when unplugged
        self.lock = threading.Lock()
        self.ok = 0
        self.fail = 0

    def esptool(self, port, *args):
        cmd = [sys.executable, "-m", "esptool", "--chip", "esp32", "-p", port,
               "-b", str(self.baud), "--before", "default_reset", *args]
        return subprocess.run(cmd, capture_output=True, text=True)

    def flash(self, port):
        t0 = time.time()
        log(f"{port}: flashing...", YELLOW)
        out = ""
        status = "FAIL"
        mac = ""
        try:
            if self.erase:
                r = self.esptool(port, "--after", "no_reset", "erase_flash")
                out += r.stdout + r.stderr
                if r.returncode != 0:
                    raise RuntimeError("erase failed")
            args = ["--after", "hard_reset", "write_flash",
                    "--flash_mode", "dio", "--flash_size", "2MB", "--flash_freq", "40m"]
            for off, path in self.images:
                args += [hex(off), path]
            r = self.esptool(port, *args)
            out += r.stdout + r.stderr
            m = re.search(r"MAC:\s*([0-9a-fA-F:]{17})", out)
            mac = eth_mac(m.group(1)) if m else ""
            if r.returncode == 0 and "Hash of data verified" in out:
                status = "OK"
        except Exception as e:  # noqa: BLE001 — report anything as a failed board
            out += str(e)

        secs = time.time() - t0
        with self.lock:
            if status == "OK":
                self.ok += 1
            else:
                self.fail += 1
            self.write_csv(port, mac, status, secs)
            totals = f"[{self.ok} ok / {self.fail} failed]"

        if status == "OK":
            log(f"{port}: OK  MAC {mac}  ({secs:.0f}s)  {totals}  — unplug and plug in the next board", GREEN)
        else:
            hint = ""
            if "Failed to connect" in out or "No serial data received" in out:
                hint = " — board didn't enter bootloader; hold BOOT, tap RST, then re-plug"
            elif "could not open port" in out or "Resource busy" in out:
                hint = " — port busy (close idf.py monitor / other serial apps)"
            last = [l for l in out.strip().splitlines() if l.strip()][-1:] or ["(no output)"]
            log(f"{port}: FAILED{hint}  {totals}\n           {last[0]}", RED)

        with self.lock:
            self.busy.discard(port)
            self.handled.add(port)

    def write_csv(self, port, mac, status, secs):
        new = not os.path.exists(self.log_path)
        with open(self.log_path, "a", newline="") as f:
            w = csv.writer(f)
            if new:
                w.writerow(["timestamp", "port", "eth_mac", "status", "seconds", "firmware"])
            w.writerow([datetime.datetime.now().isoformat(timespec="seconds"), port, mac,
                        status, f"{secs:.1f}", os.path.basename(self.images[-1][1])])

    def run(self):
        while True:
            present = candidate_ports()
            with self.lock:
                # Forget boards that were unplugged so a re-plug flashes again
                self.handled &= set(present)
                new = [p for p in present if p not in self.busy and p not in self.handled]
                self.busy.update(new)
            for port in new:
                log(f"{port}: detected ({present[port].description})")
                threading.Thread(target=self.flash, args=(port,), daemon=True).start()
            time.sleep(0.5)


def main():
    ap = argparse.ArgumentParser(description="Auto-flash PortLens boards as they are plugged in.")
    ap.add_argument("--source", choices=["auto", "build", "docs"], default="auto",
                    help="firmware from build/ (latest idf.py build) or docs/firmware/ (web flasher copy); "
                         "auto = build/ if present")
    ap.add_argument("-b", "--baud", type=int, default=921600,
                    help="flash baud rate (default 921600; try 460800 if flashing is flaky)")
    ap.add_argument("--erase", action="store_true", help="erase the whole flash first (clears NVS)")
    ap.add_argument("--log", default=os.path.join(REPO, "flash_log.csv"), help="CSV log path")
    ap.add_argument("--skip-existing", action="store_true",
                    help="don't flash boards already plugged in when the station starts")
    args = ap.parse_args()

    try:
        import esptool  # noqa: F401
    except ImportError:
        sys.exit("esptool not found — run inside the ESP-IDF env or `pip install esptool pyserial`")

    source, images = resolve_images(args.source)
    app = images[-1][1]
    built = datetime.datetime.fromtimestamp(os.path.getmtime(app)).strftime("%Y-%m-%d %H:%M")
    print(f"PortLens flash station — firmware from {source}/ (portlens.bin built {built}, "
          f"{os.path.getsize(app) // 1024} KB)")
    print(f"Logging to {args.log}. Plug in boards; Ctrl+C to quit.\n")

    station = Station(images, args.baud, args.erase, args.log)
    if args.skip_existing:
        station.handled = set(candidate_ports())
    try:
        station.run()
    except KeyboardInterrupt:
        print(f"\nDone: {station.ok} ok, {station.fail} failed.")


if __name__ == "__main__":
    main()
