#!/bin/bash
#
# run_update_benchmark.sh
# Run a WiredTiger update workload with I/O monitoring and visualization.
#
# This script:
#   1. Sets up the WiredTiger environment
#   2. Starts I/O monitoring in the background
#   3. Runs the workgen update benchmark
#   4. Stops monitoring and generates the visualization
#
# Usage:
#   ./bench/workgen/runner/run_update_benchmark.sh [OPTIONS]
#
# Options:
#   --run-time N          Benchmark duration in seconds (default: 120)
#   --num-threads N       Number of update threads (default: 16)
#   --cache-size SIZE     WiredTiger cache size (default: 4GB)
#   --ckpt-interval N     Checkpoint interval in seconds (default: 5)
#   --monitor-interval N  I/O monitoring sample interval in seconds (default: 1)
#   --no-plot             Skip visualization
#   --simple-plot         Use simple single-panel plot (default: multi-panel)
#   --help                Show this help
#
# Examples:
#   # Run 120s benchmark with default settings:
#   ./bench/workgen/runner/run_update_benchmark.sh
#
#   # Run 300s benchmark with 32 threads and 8GB cache:
#   ./bench/workgen/runner/run_update_benchmark.sh --run-time 300 --num-threads 32 --cache-size 8GB
#
#   # Run without visualization:
#   ./bench/workgen/runner/run_update_benchmark.sh --no-plot
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WT_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
BUILD_DIR="${WT_ROOT}/build"

# ── Defaults ─────────────────────────────────────────────────────────────────
RUN_TIME=120
NUM_THREADS=16
CACHE_SIZE="4GB"
CKPT_INTERVAL=5
MONITOR_INTERVAL=1
PLOT=true
SIMPLE_PLOT=false

# ── Parse arguments ─────────────────────────────────────────────────────────
while [[ $# -gt 0 ]]; do
    case "$1" in
        --run-time)          RUN_TIME="$2"; shift 2 ;;
        --num-threads)       NUM_THREADS="$2"; shift 2 ;;
        --cache-size)        CACHE_SIZE="$2"; shift 2 ;;
        --ckpt-interval)     CKPT_INTERVAL="$2"; shift 2 ;;
        --monitor-interval)  MONITOR_INTERVAL="$2"; shift 2 ;;
        --no-plot)           PLOT=false; shift ;;
        --simple-plot)       SIMPLE_PLOT=true; shift ;;
        --help)              cat <<EOF
Run WiredTiger update benchmark with I/O monitoring and visualization.

Usage: $0 [OPTIONS]

Options:
  --run-time N            Benchmark duration in seconds (default: $RUN_TIME)
  --num-threads N         Number of update threads (default: $NUM_THREADS)
  --cache-size SIZE       WiredTiger cache size (default: $CACHE_SIZE)
  --ckpt-interval N       Checkpoint interval in seconds (default: $CKPT_INTERVAL)
  --monitor-interval N    I/O monitoring sample interval (default: $MONITOR_INTERVAL)
  --no-plot               Skip visualization
  --simple-plot           Use simple single-panel plot
  --help                  Show this help

Examples:
  $0
  $0 --run-time 300 --num-threads 32 --cache-size 8GB
  $0 --no-plot
EOF
              exit 0 ;;
        *) echo "Unknown option: $1"; exit 1 ;;
    esac
done

# ── Setup ────────────────────────────────────────────────────────────────────
HOME_DIR="${BUILD_DIR}/WT_TEST_update_$(date +%Y%m%d_%H%M%S)"
MONITOR_CSV="${HOME_DIR}/io_stats.csv"
PLOT_OUTPUT="${HOME_DIR}/io_plot.png"
PLOT_OUTPUT_SIMPLE="${HOME_DIR}/io_plot_simple.png"

echo "═══════════════════════════════════════════════════════════"
echo "  WiredTiger Update Benchmark"
echo "═══════════════════════════════════════════════════════════"
echo ""
echo "  Home directory:    ${HOME_DIR}"
echo "  Run time:          ${RUN_TIME}s"
echo "  Threads:           ${NUM_THREADS}"
echo "  Cache size:        ${CACHE_SIZE}"
echo "  Checkpoint every:  ${CKPT_INTERVAL}s"
echo "  Monitor interval:  ${MONITOR_INTERVAL}s"
echo ""

# Clean up any previous run
rm -rf "${HOME_DIR}"
mkdir -p "${HOME_DIR}"

# ── Export environment ───────────────────────────────────────────────────────
export WT_BUILDDIR="${BUILD_DIR}"
export LD_LIBRARY_PATH="${BUILD_DIR}:${BUILD_DIR}/ext/compressors/zstd/.libs:${BUILD_DIR}/ext/compressors/snappy/.libs:${LD_LIBRARY_PATH:-}"
export DYLD_LIBRARY_PATH="${BUILD_DIR}:${BUILD_DIR}/ext/compressors/zstd/.libs:${BUILD_DIR}/ext/compressors/snappy/.libs:${DYLD_LIBRARY_PATH:-}"

# ── Start I/O monitoring ────────────────────────────────────────────────────
echo "Starting I/O monitoring (PID: $$)..."
python3 "${SCRIPT_DIR}/monitor_io.py" \
    --home "${HOME_DIR}" \
    --output "${MONITOR_CSV}" \
    --interval "${MONITOR_INTERVAL}" \
    --duration "$((RUN_TIME + 10))" &
MONITOR_PID=$!
echo "  Monitor PID: ${MONITOR_PID}"
echo ""

# ── Run the benchmark ───────────────────────────────────────────────────────
echo "═══════════════════════════════════════════════════════════"
echo "  Running benchmark..."
echo "═══════════════════════════════════════════════════════════"
echo ""

python3 "${SCRIPT_DIR}/update_uniform_10M.py" \
    --run-time "${RUN_TIME}" \
    --num-threads "${NUM_THREADS}" \
    --cache-size "${CACHE_SIZE}" \
    --checkpoint-interval "${CKPT_INTERVAL}"

echo ""

# ── Stop monitoring ─────────────────────────────────────────────────────────
echo "Stopping I/O monitor..."
kill ${MONITOR_PID} 2>/dev/null || true
wait ${MONITOR_PID} 2>/dev/null || true
echo "Monitor stopped."
echo ""

# ── Generate visualization ──────────────────────────────────────────────────
if [ "${PLOT}" = true ]; then
    echo "═══════════════════════════════════════════════════════════"
    echo "  Generating I/O visualization..."
    echo "═══════════════════════════════════════════════════════════"
    echo ""

    if [ "${SIMPLE_PLOT}" = true ]; then
        python3 "${SCRIPT_DIR}/viz_io.py" \
            --input "${MONITOR_CSV}" \
            --output "${PLOT_OUTPUT_SIMPLE}" \
            --checkpoint-interval "${CKPT_INTERVAL}" \
            --simple-plot
        PLOT_FILE="${PLOT_OUTPUT_SIMPLE}"
    else
        python3 "${SCRIPT_DIR}/viz_io.py" \
            --input "${MONITOR_CSV}" \
            --output "${PLOT_OUTPUT}" \
            --checkpoint-interval "${CKPT_INTERVAL}"
        PLOT_FILE="${PLOT_OUTPUT}"
    fi

    echo ""
    echo "═══════════════════════════════════════════════════════════"
    echo "  Results"
    echo "═══════════════════════════════════════════════════════════"
    echo ""
    echo "  I/O plot:       ${PLOT_FILE}"
    echo "  Raw data:       ${MONITOR_CSV}"
    echo "  Home directory: ${HOME_DIR}"
    echo ""
    echo "  To view the plot:"
    echo "    xdg-open ${PLOT_FILE}"
    echo ""
else
    echo "═══════════════════════════════════════════════════════════"
    echo "  Results"
    echo "═══════════════════════════════════════════════════════════"
    echo ""
    echo "  Raw data:       ${MONITOR_CSV}"
    echo "  Home directory: ${HOME_DIR}"
    echo ""
    echo "  To generate a plot later:"
    echo "    python3 ${SCRIPT_DIR}/viz_io.py --input ${MONITOR_CSV} --checkpoint-interval ${CKPT_INTERVAL}"
    echo ""
fi

echo "Done."
