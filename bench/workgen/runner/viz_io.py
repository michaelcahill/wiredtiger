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
Visualize I/O monitoring data from a WiredTiger benchmark run.

Reads the CSV output from monitor_io.py and produces a publication-quality
multi-panel figure showing:
  Panel 1: Write throughput (KB/s) over time
  Panel 2: Dirty page count over time
  Panel 3: Read + Write IOPS over time

Usage:
    python3 bench/workgen/runner/viz_io.py --input io_stats.csv --output io_plot.png
    python3 bench/workgen/runner/viz_io.py --input io_stats.csv --checkpoint-interval 5
    python3 bench/workgen/runner/viz_io.py --input io_stats.csv --open

The checkpoint markers (vertical dashed lines) help you see the periodic
flush pattern caused by checkpoints.
"""

import argparse
import csv
import os
import sys

import numpy as np
import matplotlib
matplotlib.use("Agg")  # Non-interactive backend
import matplotlib.pyplot as plt
import matplotlib.dates as mdates


def load_csv(path):
    """Load the monitoring CSV into numpy arrays."""
    elapsed, write_kbs, read_kbs, dirty, dirty_delta, ckpt_bytes, ckpt_delta = [], [], [], [], [], [], []
    with open(path, "r") as f:
        reader = csv.DictReader(f)
        for row in reader:
            elapsed.append(float(row["elapsed_s"]))
            write_kbs.append(float(row["disk_write_kbs"]))
            read_kbs.append(float(row["disk_read_kbs"]))
            dirty.append(int(row["dirty_pages"]))
            dirty_delta.append(int(row["dirty_pages_delta"]))
            ckpt_bytes.append(int(row["ckpt_bytes_total"]))
            ckpt_delta.append(int(row["ckpt_bytes_delta"]))
    return (np.array(elapsed), np.array(write_kbs), np.array(read_kbs),
            np.array(dirty), np.array(dirty_delta),
            np.array(ckpt_bytes), np.array(ckpt_delta))


def plot_io(elapsed, write_kbs, read_kbs, dirty, dirty_delta,
            ckpt_bytes, ckpt_delta, ckpt_interval, output):
    """Create the multi-panel visualization."""
    fig, axes = plt.subplots(4, 1, figsize=(16, 14), sharex=True,
                             gridspec_kw={"height_ratios": [2, 2, 2, 1.5]})
    fig.suptitle("WiredTiger I/O Performance During Update Workload",
                 fontsize=16, fontweight="bold", y=0.98)

    t = elapsed  # seconds

    #  Panel 1: Write throughput
    ax = axes[0]
    ax.fill_between(t, 0, write_kbs, alpha=0.4, color="#e74c3c", label="Write")
    ax.fill_between(t, 0, read_kbs, alpha=0.4, color="#3498db", label="Read")
    ax.set_ylabel("Throughput (KB/s)")
    ax.set_title("Disk I/O Throughput")
    ax.legend(loc="upper right", fontsize=11)
    ax.grid(True, alpha=0.3)
    ax.set_ylim(bottom=0)

    #  Panel 2: Dirty page count
    ax = axes[1]
    ax.plot(t, dirty, color="#27ae60", linewidth=1.5, label="Dirty pages")
    ax.plot(t, dirty_delta, color="#2ecc71", linewidth=0.8, alpha=0.6,
            label="Dirty pages (delta)")
    ax.set_ylabel("Dirty Pages")
    ax.set_title("Cache Dirty Page Count")
    ax.legend(loc="upper right", fontsize=11)
    ax.grid(True, alpha=0.3)

    #  Panel 3: IOPS
    ax = axes[2]
    ax.plot(t, read_kbs / 512, color="#3498db", linewidth=1.2, label="Read IOPS (est.)")
    ax.plot(t, write_kbs / 512, color="#e74c3c", linewidth=1.2, label="Write IOPS (est.)")
    ax.set_ylabel("IOPS (estimated)")
    ax.set_title("Disk I/O Operations Per Second (estimated from sectors)")
    ax.legend(loc="upper right", fontsize=11)
    ax.grid(True, alpha=0.3)

    #  Panel 4: Checkpoint bytes written
    ax = axes[3]
    ax.plot(t, ckpt_bytes / (1024 * 1024), color="#8e44ad", linewidth=1.5,
            label="Checkpoint bytes written (MB)")
    ax.set_ylabel("Checkpoint bytes (MB)")
    ax.set_xlabel("Elapsed Time (seconds)")
    ax.set_title("Checkpoint Data Written to Disk")
    ax.legend(loc="upper right", fontsize=11)
    ax.grid(True, alpha=0.3)

    #  Checkpoint markers
    if ckpt_interval > 0:
        ckpt_times = np.arange(ckpt_interval, t[-1], ckpt_interval)
        for ct in ckpt_times:
            ax.axvline(x=ct, color="#f39c12", linestyle="--", alpha=0.5, linewidth=0.8)
            # Also add to all panels
            for a in axes:
                a.axvline(x=ct, color="#f39c12", linestyle="--", alpha=0.3, linewidth=0.6)

        # Add a legend entry for checkpoint markers
        from matplotlib.lines import Line2D
        ckpt_legend = Line2D([0], [0], color="#f39c12", linestyle="--",
                             alpha=0.5, label=f"Checkpoint every {ckpt_interval}s")
        axes[3].legend(handles=[ckpt_legend], loc="upper right", fontsize=11)

    plt.tight_layout(rect=[0, 0, 1, 0.96])
    plt.savefig(output, dpi=150, bbox_inches="tight", facecolor="white")
    plt.close(fig)
    print(f"Plot saved to {output}")


def plot_simple(elapsed, write_kbs, read_kbs, dirty, dirty_delta,
                ckpt_bytes, ckpt_delta, ckpt_interval, output):
    """Alternative: single-panel focused on write throughput with checkpoint markers."""
    fig, ax = plt.subplots(figsize=(16, 6))
    fig.suptitle("WiredTiger Write Throughput During Update Workload",
                 fontsize=16, fontweight="bold")

    ax.fill_between(elapsed, 0, write_kbs, alpha=0.5, color="#e74c3c", label="Write KB/s")
    ax.fill_between(elapsed, 0, read_kbs, alpha=0.3, color="#3498db", label="Read KB/s")

    if ckpt_interval > 0:
        ckpt_times = np.arange(ckpt_interval, elapsed[-1], ckpt_interval)
        for ct in ckpt_times:
            ax.axvline(x=ct, color="#f39c12", linestyle="--", alpha=0.6, linewidth=1,
                       label="Checkpoint" if ct == ckpt_times[0] else "")

    ax.set_xlabel("Elapsed Time (seconds)")
    ax.set_ylabel("Throughput (KB/s)")
    ax.legend(loc="upper right", fontsize=12)
    ax.grid(True, alpha=0.3)
    ax.set_ylim(bottom=0)

    plt.tight_layout(rect=[0, 0, 1, 0.94])
    plt.savefig(output, dpi=150, bbox_inches="tight", facecolor="white")
    plt.close(fig)
    print(f"Simple plot saved to {output}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(
        description="Visualize WiredTiger I/O monitoring data")
    parser.add_argument("--input", "-i", required=True, help="Input CSV from monitor_io.py")
    parser.add_argument("--output", "-o", default="io_plot.png", help="Output PNG file")
    parser.add_argument("--checkpoint-interval", type=int, default=5,
                        help="Checkpoint interval in seconds (for markers)")
    parser.add_argument("--simple", action="store_true",
                        help="Use simple single-panel plot instead of multi-panel")
    parser.add_argument("--open", action="store_true", help="Open plot after saving")
    args = parser.parse_args()

    if not os.path.exists(args.input):
        print(f"Error: input file '{args.input}' not found.", file=sys.stderr)
        sys.exit(1)

    (elapsed, write_kbs, read_kbs, dirty, dirty_delta,
     ckpt_bytes, ckpt_delta) = load_csv(args.input)

    if args.simple:
        plot_simple(elapsed, write_kbs, read_kbs, dirty, dirty_delta,
                    ckpt_bytes, ckpt_delta, args.checkpoint_interval, args.output)
    else:
        plot_io(elapsed, write_kbs, read_kbs, dirty, dirty_delta,
                ckpt_bytes, ckpt_delta, args.checkpoint_interval, args.output)

    if args.open:
        os.system(f"xdg-open {args.output} &")
