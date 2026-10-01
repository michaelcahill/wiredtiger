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
"""Generate a self-contained WiredTigerCache explorer page.

Usage:
    cache_explorer_gen.py [-o OUT] [--sample N] [--snapshots N] PATH [PATH ...]

Each PATH is either a WiredTigerCache.* dump file or a directory, in which
case every WiredTigerCache.* file in it (sorted) is included. Snapshots from
all files are concatenated in the order given, forming one continuous
timeline. The output is a single HTML file with the data and d3 inlined;
d3.v7.min.js is downloaded next to this script on first use if missing.

By default at most --sample pages per source and snapshot are kept; when a
cache log has more, a random (but deterministic) representative subset is
used so the generated page stays small and fast. Likewise at most
--snapshots snapshots are kept (the final snapshot is always included).

Files are read twice: first to count snapshots, then to parse only the
selected ones, so memory stays bounded by the kept data no matter how large
the logs are. A truncated or malformed line (e.g. a log copied from a
running instance) is skipped with a warning; everything before it is
processed normally.
"""
import argparse
import glob
import json
import os
import random
import sys
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
D3_URL = "https://cdn.jsdelivr.net/npm/d3@7/dist/d3.min.js"
FIELDS = ["size", "dirty", "bytes_in_cache", "dirty_bytes", "update_bytes", "read_gen",
          "page_type", "pin_reason"]


def collect_files(paths):
    files = []
    for p in paths:
        if os.path.isdir(p):
            found = sorted(glob.glob(os.path.join(p, "WiredTigerCache.*")))
            if not found:
                sys.exit(f"error: no WiredTigerCache.* files in {p}")
            files += found
        elif os.path.isfile(p):
            files.append(p)
        else:
            sys.exit(f"error: no such file or directory: {p}")
    if not files:
        sys.exit("error: no input files")
    return files


def count_snapshots(files):
    """Pass 1: count non-empty lines without parsing them."""
    total = 0
    for f in files:
        with open(f) as fh:
            n = sum(1 for line in fh if line.strip())
        print(f"{f}: {n} snapshots")
        total += n
    return total


def select_snapshots(total, limit):
    """Deterministic set of snapshot indices to keep; None means keep all."""
    if not limit or total <= limit:
        return None
    # deterministic so reruns agree; the final snapshot is always kept
    keep = set(random.Random("snapshots").sample(range(total - 1), limit - 1))
    keep.add(total - 1)
    print(f"note: sampled {total} -> {len(keep)} snapshots (final snapshot kept)")
    return keep


def load_docs(files, keep):
    """Pass 2: parse only the selected snapshots."""
    docs = []
    idx = 0
    for f in files:
        with open(f) as fh:
            for lineno, line in enumerate(fh, 1):
                if not line.strip():
                    continue
                if keep is None or idx in keep:
                    try:
                        docs.append(json.loads(line))
                    except json.JSONDecodeError:
                        # e.g. the last line of a log copied from a running instance
                        print(f"warning: {f}:{lineno}: skipping malformed line "
                              f"(incomplete log?); earlier snapshots are unaffected")
                idx += 1
    return docs


def main():
    usage = "\n".join(__doc__.splitlines()[2:])
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter,
                                 epilog=usage)
    ap.add_argument("paths", nargs="+",
                    help="WiredTigerCache.* files and/or directories containing them")
    ap.add_argument("-o", "--out", default="cache_explorer.html",
                    help="output HTML file (default: %(default)s)")
    ap.add_argument("--sample", type=int, default=10000,
                    help="max pages to keep per source and snapshot; excess pages are randomly "
                         "down-sampled (0 = no limit; default: %(default)s)")
    ap.add_argument("--snapshots", type=int, default=20,
                    help="max snapshots to keep; excess snapshots are randomly down-sampled, the "
                         "final snapshot is always kept (0 = no limit; default: %(default)s)")
    args = ap.parse_args()
    if args.sample < 0:
        sys.exit("error: --sample must be >= 0")
    if args.snapshots < 0:
        sys.exit("error: --snapshots must be >= 0")

    files = collect_files(args.paths)
    total = count_snapshots(files)
    keep = select_snapshots(total, args.snapshots)
    docs = load_docs(files, keep)
    sources = sorted({k for d in docs for k in d["wiredTigerCache"]})

    # data[source] = [snapshot, size, dirty, bytes_in_cache, dirty_bytes, update_bytes, read_gen,
    #                 page_type, pin_reason]
    data = {}
    missing = set()
    for s in sources:
        rows = []
        for snap, d in enumerate(docs):
            pages = d["wiredTigerCache"].get(s, [])
            if args.sample and len(pages) > args.sample:
                # deterministic per (source, snapshot) so reruns agree; keep walk order
                keep = sorted(random.Random(f"{s}:{snap}").sample(range(len(pages)), args.sample))
                print(f"note: {s} @ snapshot {snap}: sampled {len(pages)} -> {args.sample} pages")
                pages = [pages[i] for i in keep]
            for r in pages:
                if not all(f in r for f in FIELDS):
                    missing |= set(FIELDS) - set(r)
                rows.append([snap] + [r.get(f) for f in FIELDS])
        data[s] = rows
    if missing:
        print(f"warning: some samples lack fields {sorted(missing)}; they will render as undefined")

    payload = json.dumps({"fields": FIELDS, "sources": sources,
                          "times": [d["localTime"] for d in docs], "data": data},
                         separators=(",", ":"))

    # d3 is inlined so the output is a single portable file
    d3path = os.path.join(HERE, "d3.v7.min.js")
    if not os.path.exists(d3path):
        print(f"downloading d3 to {d3path} ...")
        urllib.request.urlretrieve(D3_URL, d3path)
    D3 = open(d3path).read()
    assert "</script" not in D3.lower(), "d3 build cannot be inlined safely"

    tpl = open(os.path.join(HERE, "cache_explorer_template.html")).read()
    html = tpl.replace("__DATA__", payload).replace("__D3__", D3)
    with open(args.out, "w") as fh:
        fh.write(html)
    counts = {s: len(v) for s, v in data.items()}
    print(f"{args.out}: {len(docs)} snapshots, sources={sources}, rows={counts}")


if __name__ == "__main__":
    main()
