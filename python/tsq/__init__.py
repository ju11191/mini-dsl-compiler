"""tsq: a tiny pipeline query language for columnar time-series, JIT-compiled with LLVM.

>>> import tsq
>>> data = tsq.datagen.make_dataset(100_000)
>>> q = tsq.compile('trades | filter price > 100 and sym == "AAPL" | window 5m mean(price)')
>>> out = q.run(data.tables, symbols=data.symbols)   # dict[str, np.ndarray]
"""

from __future__ import annotations

from . import datagen
from ._core import CompileError, DataError
from .query import Query, compile, infer_schema, to_pandas

__all__ = [
    "CompileError",
    "DataError",
    "Query",
    "compile",
    "datagen",
    "infer_schema",
    "to_pandas",
]
