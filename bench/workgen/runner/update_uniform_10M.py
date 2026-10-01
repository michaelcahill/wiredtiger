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
# Update workload: 10M keys, 100-byte values, uniform random key selection.
# Checkpoints every 5 seconds.
#
# Usage:
#   cd /home/mjc/work/wiredtiger/build
#   WT_BUILDDIR=$(pwd) python3 bench/workgen/runner/update_uniform_10M.py
#
# Or with custom parameters:
#   WT_BUILDDIR=$(pwd) python3 bench/workgen/runner/update_uniform_10M.py \
#       --run-time 120 --num-threads 16 --cache-size 4GB --checkpoint-interval 5
#

import sys, os

#  Parse our custom command-line overrides FIRST
# Must happen before importing workgen/runner which parse sys.argv via argparse
run_time       = 120   # seconds
num_threads    = 16
cache_size     = "4GB"
ckpt_interval  = 5      # seconds
num_keys       = 10_000_000
value_size     = 100
key_size       = 8       # 8-byte integer keys fit in 8 bytes (binary format)

_custom_args = {"--run-time", "--num-threads", "--cache-size",
                "--checkpoint-interval", "--help"}
_filtered_argv = [sys.argv[0]]  # keep program name
j = 1
while j < len(sys.argv):
    arg = sys.argv[j]
    if arg in _custom_args:
        if arg == "--help":
            print(__doc__)
            sys.exit(0)
        # Skip the arg and its value
        if j + 1 < len(sys.argv) and not sys.argv[j + 1].startswith("--"):
            val = sys.argv[j + 1]
            if arg == "--run-time":
                run_time = int(val)
            elif arg == "--num-threads":
                num_threads = int(val)
            elif arg == "--cache-size":
                cache_size = val
            elif arg == "--checkpoint-interval":
                ckpt_interval = int(val)
            j += 2
        else:
            j += 1
    else:
        _filtered_argv.append(arg)
        j += 1
sys.argv = _filtered_argv  # strip our args before workgen sees them

from runner import *
from wiredtiger import *
from workgen import *

#  Create context (after sys.argv is cleaned)
context = Context()

#  Connection config
conn_config = (
    f"create,"
    f"cache_size={cache_size},"
    "eviction=(threads_max=8,threads_min=4),"
    "log=(enabled=true),"
    "statistics=(fast),"
    "statistics_log=(wait=1,json=true)"
)
conn = context.wiredtiger_open(conn_config)
s = conn.open_session()

#  Create table
tname = "table:update_test"
# Use 'u' (unsigned integer) key format for compact 8-byte keys
s.create(tname,
    "key_format=u,value_format=S,"
    "exclusive=true,"
    "allocation_size=4kb,internal_page_max=64kb,"
    "memory_page_max=10m,leaf_value_max=64MB,"
    "checksum=on,split_pct=90,type=file,log=(enabled=false)")

table = Table(tname)
table.options.key_size = key_size
table.options.value_size = value_size

#  Populate 10M keys
print(f"Populating {num_keys} keys with {value_size}-byte values...")
pop_ops = Operation(Operation.OP_INSERT, table)
# Use KEYGEN_APPEND so each insert gets the next sequential key
pop_thread = Thread(pop_ops)
pop_workload = Workload(context, pop_thread * num_keys)
pop_workload.options.report_interval = 10
ret = pop_workload.run(conn)
assert ret == 0, f"Populate failed: {ret}"
print("Populate complete.")

#  Update workload: uniform random key selection
# Use KEYGEN_PARETO with param=100  uniform distribution across all keys
# range_low=0.0, range_high=1.0  full range
uniform_key = Key(Key.KEYGEN_PARETO, num_keys,
                  ParetoOptions(100))  # 100 = uniform
uniform_key._pareto.range_low = 0.0
uniform_key._pareto.range_high = 1.0

# Generate a 100-byte value
# workgen's Value class: each character in the format string is a byte
# 'x' repeated 100 times = 100 bytes of 'x'
value_str = "x" * value_size
update_val = Value(value_size)

# Update operation with uniform random key
update_op = Operation(Operation.OP_UPDATE, table, uniform_key, update_val)
update_op = txn(update_op, 'isolation=snapshot')
update_op.transaction.use_commit_timestamp = True

# Checkpoint operation: sleep + checkpoint, repeated
ckpt_op = Operation(Operation.OP_SLEEP, str(ckpt_interval)) + \
          Operation(Operation.OP_CHECKPOINT, "")
ckpt_thread = Thread(ckpt_op)

# Build the workload
update_thread = Thread(update_op)
update_thread.options.session_config = "isolation=snapshot"

workload = Workload(context,
                    num_threads * update_thread +
                    ckpt_thread)

workload.options.run_time = run_time
workload.options.report_interval = 5
workload.options.max_latency = 10000  # 10 seconds
workload.options.sample_rate = 1
workload.options.warmup = 0
workload.options.sample_interval_ms = 1000

print(f"\nRunning update workload:")
print(f"  Keys:        {num_keys}")
print(f"  Value size:  {value_size} bytes")
print(f"  Threads:     {num_threads}")
print(f"  Cache:       {cache_size}")
print(f"  Run time:    {run_time}s")
print(f"  Checkpoint:  every {ckpt_interval}s")
print(f"  Key dist:    uniform random\n")

ret = workload.run(conn)
assert ret == 0, f"Workload failed: {ret}"

#  Print final stats
print("\n=== Final Statistics ===")
stats = conn.get_stats()
for name, val in stats.items():
    if val != 0:
        print(f"  {name:60s} = {val}")

cache_eviction_file = context.args.home + "/cache_eviction.stat"
get_cache_eviction_stats(s, cache_eviction_file)
print(f"\nCache eviction stats written to {cache_eviction_file}")

latency_filename = context.args.home + "/latency.stat"
latency.workload_latency(workload, latency_filename)
print(f"Latency stats written to {latency_filename}")

conn.close()
print("\nDone.")
