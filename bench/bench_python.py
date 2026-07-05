"""End-to-end Python benchmark: tsq (JIT / interpreter) vs pandas on the same queries.

    .venv/bin/python bench/bench_python.py --rows 10000000 --reps 3

Times include the Python call overhead and input validation (validate=True),
but not JIT compilation (reported separately). pandas is given the data
already converted to DataFrames, with sym codes kept as int32.
"""

from __future__ import annotations

import argparse
import platform
import statistics
import sys
import time
from pathlib import Path

import numpy as np
import pandas as pd

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "python"))
import tsq  # noqa: E402

FIVE_MIN = 300_000_000_000


def pandas_filter(tr, aapl):
    return tr[(tr.price > 100) & (tr.sym == aapl)]


def pandas_window(tr):
    df = tr.assign(__dt=pd.to_datetime(tr.ts, unit="ns"))
    r = df.rolling(f"{FIVE_MIN}ns", on="__dt")
    return df.assign(
        sum_size=r["size"].sum(),
        mean_price=r["price"].mean(),
        min_price=r["price"].min(),
        max_price=r["price"].max(),
    ).drop(columns="__dt")


def pandas_asof(tr, qu):
    return pd.merge_asof(tr, qu, on="ts", by="sym", direction="backward")


def pandas_combined(tr, qu, aapl):
    f = pandas_filter(tr, aapl)
    f = f.assign(__dt=pd.to_datetime(f.ts, unit="ns"))
    f = f.assign(mean_price=f.rolling(f"{FIVE_MIN}ns", on="__dt")["price"].mean()).drop(
        columns="__dt"
    )
    return pd.merge_asof(f, qu, on="ts", by="sym", direction="backward")


QUERIES = {
    "filter": 'trades | filter price > 100 and sym == "AAPL"',
    "window": "trades | window 5m sum(size), mean(price), min(price), max(price)",
    "asof": "trades | asof_join quotes on sym",
    "combined": 'trades | filter price > 100 and sym == "AAPL" | window 5m mean(price) '
    "| asof_join quotes on sym",
}


def timeit(fn, reps):
    out, times = None, []
    for _ in range(reps):
        t0 = time.perf_counter()
        out = fn()
        times.append((time.perf_counter() - t0) * 1000)
    return out, times


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rows", type=int, default=10_000_000)
    ap.add_argument("--reps", type=int, default=3)
    ap.add_argument("--no-interp", action="store_true")
    args = ap.parse_args()

    print(f"# host: {platform.machine()} {platform.platform()} python {platform.python_version()}")
    print(f"# numpy {np.__version__} pandas {pd.__version__}; rows={args.rows} reps={args.reps}")
    t0 = time.perf_counter()
    data = tsq.datagen.make_dataset(args.rows, seed=0)
    print(f"# datagen {time.perf_counter() - t0:.1f}s")
    tr = pd.DataFrame(data.tables["trades"])
    qu = pd.DataFrame(data.tables["quotes"])
    aapl = data.symbols.index("AAPL")
    ref = {
        "filter": lambda: pandas_filter(tr, aapl),
        "window": lambda: pandas_window(tr),
        "asof": lambda: pandas_asof(tr, qu),
        "combined": lambda: pandas_combined(tr, qu, aapl),
    }
    hdr = f"{'query':<10} {'rows_out':>10} {'jit_ms':>9} {'interp_ms':>10} {'pandas_ms':>10}"
    print(hdr + f" {'compile_ms':>10} {'pandas/jit':>10}")
    for name, text in QUERIES.items():
        q = tsq.compile(text)
        out, tj = timeit(lambda q=q: q.run(data.tables, data.symbols), args.reps)
        compile_ms = q.compile_stats["total_ms"]
        ti = [0.0]
        if not args.no_interp:
            qi = tsq.compile(text, backend="interp")
            _, ti = timeit(lambda qi=qi: qi.run(data.tables, data.symbols), args.reps)
        pout, tp = timeit(ref[name], args.reps)
        assert len(pout) == len(out["ts"]), (name, len(pout), len(out["ts"]))
        mj, mi, mp = statistics.median(tj), statistics.median(ti), statistics.median(tp)
        print(
            f"{name:<10} {len(out['ts']):>10} {mj:>9.1f} {mi:>10.1f} {mp:>10.1f} "
            f"{compile_ms:>10.2f} {mp / mj:>10.1f}"
        )
        print(f"#raw {name} jit={tj} interp={ti} pandas={tp}")


if __name__ == "__main__":
    main()
