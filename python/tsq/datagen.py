"""Synthetic trades/quotes generator (numpy, vectorized).

trades: ts (int64 ns, ascending), sym (int32 codes), price (float64), size (int64)
quotes: ts (int64 ns, ascending), sym (int32 codes), bid (float64), ask (float64)

Prices follow a per-symbol mean-reverting AR(1) process around a base price,
matching the shape of the C++ generator in src/datagen.cpp (not bit-identical).
"""

from __future__ import annotations

from dataclasses import dataclass

import numpy as np

SYMBOLS = ["AAPL", "MSFT", "GOOG", "AMZN", "META", "NVDA", "TSLA", "JPM"]
_BASE = np.array([100, 400, 140, 180, 500, 120, 250, 95], dtype=np.float64)
START_NS = 1704205800 * 1_000_000_000  # 2024-01-02 09:30 New York


def _base_prices(nsym: int) -> np.ndarray:
    extra = 50.0 + 25.0 * np.arange(len(_BASE), max(nsym, len(_BASE)))
    return np.concatenate([_BASE, extra])[:nsym]


def _ar1(noise: np.ndarray, r: float, block: int = 1024) -> np.ndarray:
    """y[t] = r*y[t-1] + noise[t], y[-1] = 0, vectorized in blocks."""
    n = len(noise)
    if n == 0:
        return noise.copy()
    nb = -(-n // block)
    e = np.zeros(nb * block)
    e[:n] = noise
    e = e.reshape(nb, block)
    j = np.arange(block)
    # Within a block (y0 = 0): y[j] = r^j * cumsum(e[k] * r^-k)
    part = np.cumsum(e * r ** (-j), axis=1) * r**j
    out = np.empty_like(part)
    carry = 0.0
    rj1 = r ** (j + 1)
    for b in range(nb):  # sequential only across blocks
        out[b] = part[b] + rj1 * carry
        carry = out[b, -1]
    return out.reshape(-1)[:n]


def _timestamps(rng: np.random.Generator, n: int, start: int) -> np.ndarray:
    gaps = rng.integers(0, 2001, size=n, dtype=np.int64) * 1000  # 0..2ms, mean 1ms
    return start + np.cumsum(gaps)


def _prices(rng: np.random.Generator, sym: np.ndarray, nsym: int) -> np.ndarray:
    base = _base_prices(nsym)
    px = np.empty(len(sym), dtype=np.float64)
    for s in range(nsym):
        idx = np.flatnonzero(sym == s)
        noise = base[s] * 0.002 * (rng.random(len(idx)) - 0.5)
        px[idx] = base[s] + _ar1(noise, 0.999)
    return px


def trades(n: int, seed: int = 0, nsym: int = 8) -> dict[str, np.ndarray]:
    rng = np.random.default_rng(seed)
    ts = _timestamps(rng, n, START_NS)
    sym = rng.integers(0, nsym, size=n, dtype=np.int32)
    price = np.round(_prices(rng, sym, nsym), 2)
    size = rng.integers(1, 1001, size=n, dtype=np.int64)
    return {"ts": ts, "sym": sym, "price": price, "size": size}


def quotes(n: int, seed: int = 1, nsym: int = 8) -> dict[str, np.ndarray]:
    rng = np.random.default_rng(seed + 0x51AB1E5)
    ts = _timestamps(rng, n, START_NS - 5_000_000)  # start 5ms before trades
    sym = rng.integers(0, nsym, size=n, dtype=np.int32)
    mid = _prices(rng, sym, nsym)
    half = 0.01 * rng.integers(1, 6, size=n)
    return {"ts": ts, "sym": sym, "bid": np.round(mid - half, 2), "ask": np.round(mid + half, 2)}


@dataclass
class Dataset:
    tables: dict[str, dict[str, np.ndarray]]
    symbols: list[str]


def symbols(nsym: int = 8) -> list[str]:
    return SYMBOLS[:nsym] + [f"SYM{i}" for i in range(len(SYMBOLS), nsym)]


def make_dataset(
    n_trades: int, n_quotes: int | None = None, seed: int = 0, nsym: int = 8
) -> Dataset:
    """trades + quotes + the shared symbol dictionary."""
    nq = n_trades if n_quotes is None else n_quotes
    return Dataset(
        tables={"trades": trades(n_trades, seed, nsym), "quotes": quotes(nq, seed + 1, nsym)},
        symbols=symbols(nsym),
    )
