#!/usr/bin/env python3
"""
dht22_read.py – read temperature and humidity from /dev/dht22
=============================================================

Usage:
  sudo python3 dht22_read.py               # print one reading
  sudo python3 dht22_read.py --loop 5      # read every 5 seconds
  sudo python3 dht22_read.py --loop 5 --csv readings.csv

Requires dht22_driver.ko to be loaded:
  cd driver && make load
"""

import argparse
import csv
import signal
import sys
import time
from datetime import datetime, timezone
from pathlib import Path

DEVICE = "/dev/dht22"


def read_sensor(device: str) -> dict:
    """
    Read one line from /dev/dht22 and return a parsed dict.

    The kernel driver returns:  "<temp_x10> <rh_x10> <timestamp_ns>\\n"
    e.g.                        "234 612 1716400000123456789\\n"
    """
    with open(device, "r") as f:
        line = f.readline().strip()

    parts = line.split()
    if len(parts) != 3:
        raise ValueError(f"unexpected format from driver: {line!r}")

    temp_x10 = int(parts[0])
    rh_x10   = int(parts[1])
    ts_ns    = int(parts[2])

    return {
        "temperature_c":  temp_x10 / 10.0,
        "temperature_f":  temp_x10 / 10.0 * 9 / 5 + 32,
        "humidity_pct":   rh_x10   / 10.0,
        "kernel_ts_ns":   ts_ns,
        "wall_time":      datetime.now(timezone.utc),
    }


def print_reading(r: dict) -> None:
    wall = r["wall_time"].strftime("%Y-%m-%d %H:%M:%S UTC")
    print(f"[{wall}]")
    print(f"  Temperature : {r['temperature_c']:.1f} °C  "
          f"({r['temperature_f']:.1f} °F)")
    print(f"  Humidity    : {r['humidity_pct']:.1f} %RH")
    print(f"  Kernel ts   : {r['kernel_ts_ns']:,} ns")
    print()


def check_device(device: str) -> None:
    p = Path(device)
    if not p.exists():
        sys.exit(
            f"ERROR: {device} not found.\n"
            f"       Load the module first:  cd driver && make load"
        )
    import os
    if not os.access(device, os.R_OK):
        sys.exit(
            f"ERROR: no read permission on {device}.\n"
            f"       Run with sudo."
        )


def main() -> None:
    parser = argparse.ArgumentParser(description="Read DHT22 via /dev/dht22")
    parser.add_argument("--device", default=DEVICE)
    parser.add_argument("--loop", type=float, metavar="SECONDS",
                        help="poll continuously, sleeping SECONDS between reads")
    parser.add_argument("--csv", metavar="FILE",
                        help="append readings to a CSV file")
    args = parser.parse_args()

    check_device(args.device)

    log_file   = None
    log_writer = None
    if args.csv:
        log_file = open(args.csv, "a", newline="")
        log_writer = csv.writer(log_file)
        if log_file.tell() == 0:
            log_writer.writerow(
                ["wall_time", "temperature_c", "humidity_pct", "kernel_ts_ns"]
            )

    def _exit(sig, frame):
        print("\nStopped.")
        if log_file:
            log_file.close()
        sys.exit(0)

    signal.signal(signal.SIGINT, _exit)

    while True:
        try:
            r = read_sensor(args.device)
            print_reading(r)
            if log_writer:
                log_writer.writerow([
                    r["wall_time"].isoformat(),
                    r["temperature_c"],
                    r["humidity_pct"],
                    r["kernel_ts_ns"],
                ])
                log_file.flush()
        except Exception as e:
            print(f"Read error: {e}", file=sys.stderr)

        if args.loop is None:
            break
        time.sleep(args.loop)


if __name__ == "__main__":
    main()
