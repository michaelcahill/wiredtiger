#!/usr/bin/env python3
#
# Public Domain 2014-present MongoDB, Inc.
# Public Domain 2008-2014 WiredTiger, Inc.
#
# This is free and unencumbered software released into the public domain.
#
# Anyone is free to copy, modify, publish, use, compile, sell, or
# distribute this software, either in source code form or as a compiled
# binary, for any purpose, commercial or non-commercial, and by any
# means.
#
# In jurisdictions that recognize copyright laws, the author or authors
# of this software dedicate any and all copyright interest in the
# software to the public domain. We make this dedication for the benefit
# of the public at large and to the detriment of our heirs and
# successors. We intend this dedication to be an overt act of
# relinquishment in perpetuity of all present and future rights to this
# software under copyright law.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
# EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
# MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
# IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
# OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
# ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
# OTHER DEALINGS IN THE SOFTWARE.
#
"""
Monitor I/O statistics from /proc/diskstats and WiredTiger stats during a benchmark run.

Usage:
    python3 bench/workgen/runner/monitor_io.py --home WT_TEST --output io_stats.csv --interval 1

This reads /proc/diskstats every `interval` seconds and also polls WiredTiger
fast statistics (dirty pages, checkpoint bytes written) if the home directory
contains a running WiredTiger instance.

Output is a CSV file suitable for plotting with viz_io.py.
"""

import argparse
import csv
import os
import sys
import time

def read_diskstats():
    """Parse /proc/diskstats and return dict of disk -> {reads, writes, ...}."""
    disks = {}
    try:
        with open("/proc/diskstats", "r") as f:
            for line in f:
                parts = line.strip().split()
                if len(parts) < 14:
                    continue
                name = parts[2]
                # Skip loop/ram devices
                if name.startswith(("loop", "ram", "dm-")):
                    continue
                disks[name] = {
                    "reads_completed": int(parts[3]),
                    "reads_merged": int(parts[4]),
                    "sectors_read": int(parts[5]),
                    "read_time_ms": int(parts[6]),
                    "writes_completed": int(parts[7]),
                    "writes_merged": int(parts[8]),
                    "sectors_written": int(parts[9]),
                    "write_time_ms": int(parts[10]),
                    "io_in_progress": int(parts[11]),
                    "io_time_ms": int(parts[12]),
                    "io_time_weighted_ms": int(parts[13]),
                }
    except FileNotFoundError:
        print("Warning: /proc/diskstats not found", file=sys.stderr)
    return disks


def get_wt_dirty_pages(home):
    """Try to read dirty page count from WiredTiger stats file."""
    stats_file = os.path.join(home, "WiredTigerStats")
    dirty = 0
    ckpt_bytes = 0
    try:
        with open(stats_file, "r") as f:
            for line in f:
                if "cache dirty page clean" in line:
                    parts = line.split()
                    if len(parts) >= 4:
                        dirty = int(parts[3])
                if "checkpoint bytes written" in line:
                    parts = line.split()
                    if len(parts) >= 4:
                        ckpt_bytes = int(parts[3])
    except (FileNotFoundError, ValueError):
        pass
    return dirty, ckpt_bytes


def monitor_io(home=None, output="io_stats.csv", interval=1, duration=None):
    """Monitor I/O and write to CSV."""
    start_time = time.time()
    prev_disks = None
    prev_dirty = 0
    prev_ckpt_bytes = 0

    with open(output, "w", newline="") as csvfile:
        writer = csv.writer(csvfile)
        writer.writerow([
            "elapsed_s", "timestamp",
            "disk_read_iops", "disk_write_iops",
            "disk_read_kbs", "disk_write_kbs",
            "dirty_pages", "dirty_pages_delta",
            "ckpt_bytes_total", "ckpt_bytes_delta",
        ])

        while True:
            now = time.time()
            elapsed = now - start_time

            if duration and elapsed >= duration:
                break

            disks = read_diskstats()
            dirty, ckpt_bytes = get_wt_dirty_pages(home) if home else (0, 0)

            # Calculate deltas
            if prev_disks is not None:
                dt = elapsed - (prev_disks.get("_elapsed", elapsed))
                if dt <= 0:
                    dt = 1.0

                total_reads = sum(d["reads_completed"] for d in prev_disks.values())
                total_writes = sum(d["writes_completed"] for d in prev_disks.values())
                total_reads_new = sum(d["reads_completed"] for d in disks.values())
                total_writes_new = sum(d["writes_completed"] for d in disks.values())

                read_iops = (total_reads_new - total_reads) / dt
                write_iops = (total_writes_new - total_writes) / dt

                total_sectors_read = sum(d["sectors_read"] for d in prev_disks.values())
                total_sectors_written = sum(d["sectors_written"] for d in prev_disks.values())
                total_sectors_read_new = sum(d["sectors_read"] for d in disks.values())
                total_sectors_written_new = sum(d["sectors_written"] for d in disks.values())

                read_kbs = (total_sectors_read_new - total_sectors_read) * 512 / 1024 / dt
                write_kbs = (total_sectors_written_new - total_sectors_written) * 512 / 1024 / dt

                dirty_delta = dirty - prev_dirty
                ckpt_delta = ckpt_bytes - prev_ckpt_bytes

                writer.writerow([
                    round(elapsed, 1),
                    time.strftime("%H:%M:%S"),
                    round(read_iops, 1),
                    round(write_iops, 1),
                    round(read_kbs, 1),
                    round(write_kbs, 1),
                    dirty,
                    dirty_delta,
                    ckpt_bytes,
                    ckpt_delta,
                ])

            prev_disks = disks
            prev_disks["_elapsed"] = elapsed
            prev_dirty = dirty
            prev_ckpt_bytes = ckpt_bytes

            time.sleep(interval)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Monitor I/O for WiredTiger benchmark")
    parser.add_argument("--home", default=None, help="WiredTiger home directory")
    parser.add_argument("--output", default="io_stats.csv", help="Output CSV file")
    parser.add_argument("--interval", type=float, default=1, help="Sample interval in seconds")
    parser.add_argument("--duration", type=int, default=None, help="Run duration in seconds (None = until Ctrl+C)")
    args = parser.parse_args()
    monitor_io(args.home, args.output, args.interval, args.duration)
