"""Differential tests: JIT vs interpreter (bit-exact) vs a pandas reference.

Random data x random queries. Any mismatch prints the query text and the seed
so it can be replayed with `pytest -k ... --seed` style debugging.
"""

from __future__ import annotations

import os
import random
import warnings

import numpy as np
import pytest
import tsq
from qgen import QueryGen

N_QUERIES = int(os.environ.get("TSQ_DIFF_QUERIES", "300"))
BASE_TYPES = {"ts": "i64", "sym": "sym", "price": "f64", "size": "i64"}


def dataset(kind: str, seed: int):
    rng = np.random.default_rng(seed)
    if kind == "normal":
        d = tsq.datagen.make_dataset(3000, 2500, seed=seed, nsym=4)
        return d.tables, d.symbols
    if kind == "dupes":
        # Tiny timestamps with many duplicates: stresses window/asof tie rules.
        n, m = 800, 600
        ts = np.cumsum(rng.integers(0, 3, n)).astype(np.int64)
        qts = np.cumsum(rng.integers(0, 3, m)).astype(np.int64) - 2
        syms = ["AAPL", "MSFT", "GOOG"]
        trades = {
            "ts": ts,
            "sym": rng.integers(0, 3, n, dtype=np.int32),
            "price": np.round(rng.normal(100, 5, n), 2),
            "size": rng.integers(1, 1001, n, dtype=np.int64),
        }
        quotes = {
            "ts": qts,
            "sym": rng.integers(0, 3, m, dtype=np.int32),
            "bid": np.round(rng.normal(100, 5, m), 2),
            "ask": np.round(rng.normal(101, 5, m), 2),
        }
        return {"trades": trades, "quotes": quotes}, syms
    if kind == "sparse_quotes":
        # Few quotes, some symbols never quoted -> many NaN payloads.
        d = tsq.datagen.make_dataset(2000, 30, seed=seed, nsym=6)
        return d.tables, d.symbols
    raise ValueError(kind)


def compare(name, got, ref, exact):
    assert len(got) == len(ref), f"{name}: length {len(got)} vs {len(ref)}"
    if exact:
        np.testing.assert_array_equal(got, ref, err_msg=name)
        return
    got = got.astype(np.float64)
    ref = ref.astype(np.float64)
    finite = np.abs(ref[np.isfinite(ref)])
    scale = 1.0 + (finite.max() if len(finite) else 0.0)
    np.testing.assert_allclose(got, ref, rtol=1e-6, atol=1e-9 * scale, equal_nan=True, err_msg=name)


@pytest.mark.parametrize("case", range(N_QUERIES))
def test_random_query(case):
    kind = ["normal", "normal", "dupes", "sparse_quotes"][case % 4]
    tables, symbols = dataset(kind, seed=1000 + case // 4)
    ts = tables["trades"]["ts"]
    gen = QueryGen(random.Random(case), symbols, int(ts.min()), int(ts.max()))
    text, reference = gen.query(BASE_TYPES)
    ctx = f"\ncase={case} data={kind}\nquery: {text}"

    jit = tsq.compile(text, backend="jit").run(tables, symbols)
    interp = tsq.compile(text, backend="interp").run(tables, symbols)
    with warnings.catch_warnings(), np.errstate(all="ignore"):
        warnings.simplefilter("ignore")
        ref = reference(tables)

    assert list(jit) == list(interp) == list(ref.cols), f"columns differ{ctx}"
    for c in jit:
        # JIT and interpreter must agree bit-for-bit (NaN == NaN).
        np.testing.assert_array_equal(jit[c], interp[c], err_msg=f"jit vs interp, column {c}{ctx}")
        assert jit[c].dtype == {"i64": np.int64, "f64": np.float64, "sym": np.int32}[ref.types[c]]
        # pandas computes window aggregates in float with different algorithms
        # (Kahan sums), so those columns are compared with a tolerance.
        is_exact = ref.cols[c].dtype != np.float64 and jit[c].dtype != np.float64
        compare(f"jit vs pandas, column {c}{ctx}", jit[c], ref.cols[c], is_exact)
