"""Python front of the native core: schema inference and per-schema caching."""

from __future__ import annotations

from collections.abc import Mapping, Sequence

import numpy as np

from . import _core

Schema = dict[str, list[tuple[str, str]]]

_DTYPE_TO_TYPE = {np.dtype(np.int64): "i64", np.dtype(np.float64): "f64", np.dtype(np.int32): "sym"}


def _type_of(name: str, arr: np.ndarray) -> str:
    try:
        return _DTYPE_TO_TYPE[np.asarray(arr).dtype]
    except KeyError:
        raise TypeError(
            f"column {name!r} has dtype {np.asarray(arr).dtype}; expected int64 (i64), "
            "float64 (f64) or int32 (sym codes)"
        ) from None


def infer_schema(tables: Mapping[str, Mapping[str, np.ndarray]]) -> Schema:
    """Schema from numpy dtypes: int64 -> i64, float64 -> f64, int32 -> sym."""
    return {
        t: [(c, _type_of(f"{t}.{c}", a)) for c, a in cols.items()] for t, cols in tables.items()
    }


class Query:
    """A compiled query. Native code is generated once per input schema."""

    def __init__(
        self, text: str, schema: Schema | None, backend: str, opt_level: int, options: dict
    ):
        if backend not in ("jit", "interp"):
            raise ValueError("backend must be 'jit' or 'interp'")
        self.text = text
        self.backend = backend
        self.opt_level = opt_level
        self.options = options
        # Syntax errors surface immediately, even before a schema is known.
        self.canonical, self.tables = _core.parse(text)
        self._cache: dict[tuple, _core.Query] = {}
        self._last: _core.Query | None = None
        if schema is not None:
            self._native(schema)

    def _native(self, schema: Schema) -> _core.Query:
        relevant = {t: list(schema[t]) for t in self.tables if t in schema}
        key = tuple(sorted((t, tuple(cols)) for t, cols in relevant.items()))
        q = self._cache.get(key)
        if q is None:
            q = _core.compile(self.text, relevant, self.backend, self.opt_level, **self.options)
            self._cache[key] = q
        self._last = q
        return q

    def run(
        self,
        tables: Mapping[str, Mapping[str, np.ndarray]] | Mapping[str, np.ndarray],
        symbols: Sequence[str] = (),
        validate: bool = True,
    ) -> dict[str, np.ndarray]:
        """Run on numpy columns. `tables` maps table name -> {column -> array}.

        If the query only reads one table, a plain {column -> array} dict is also
        accepted. sym columns are int32 codes into `symbols`.
        """
        if tables and all(isinstance(v, np.ndarray) for v in tables.values()):
            tables = {self.tables[0]: tables}
        for t in self.tables:
            if t not in tables:
                raise KeyError(f"missing input table {t!r}")
        schema = infer_schema({t: tables[t] for t in self.tables if t in tables})
        return self._native(schema).run(dict(tables), list(symbols), validate)

    def _require_compiled(self) -> _core.Query:
        if self._last is None:
            raise RuntimeError("query has not been compiled yet: pass schema= or call run() first")
        return self._last

    def ir(self, optimized: bool = True) -> str:
        """LLVM IR of the most recently used native query (JIT backend)."""
        return self._require_compiled().ir(optimized)

    @property
    def compile_stats(self) -> dict[str, float]:
        return self._require_compiled().stats

    @property
    def output_schema(self) -> list[tuple[str, str]]:
        return self._require_compiled().output_schema

    def __repr__(self) -> str:
        return f"tsq.Query({self.text!r}, backend={self.backend!r})"


def compile(
    query: str,
    schema: Schema | None = None,
    backend: str = "jit",
    opt_level: int = 2,
    *,
    window_direct_input: bool = True,
    window_output_history: bool = True,
) -> Query:  # noqa: A001 - the spec asks for tsq.compile
    """Compile a query.

    schema: {table: [(column, "i64"|"f64"|"sym"), ...]}. If omitted it is inferred
    from the numpy dtypes on the first run() (and cached per schema).
    opt_level: 0 skips the LLVM optimization pipeline, 2 runs the default -O2 one.
    window_*: JIT optimizations, exposed for testing/benchmarking.
    """
    options = {
        "window_direct_input": window_direct_input,
        "window_output_history": window_output_history,
    }
    return Query(query, schema, backend, opt_level, options)


def to_pandas(result: Mapping[str, np.ndarray], symbols: Sequence[str] | None = None):
    """Convert a result dict to a pandas DataFrame (sym codes -> strings if symbols given)."""
    import pandas as pd

    cols = {}
    for name, arr in result.items():
        if symbols is not None and arr.dtype == np.int32:
            cols[name] = pd.Categorical.from_codes(arr, categories=list(symbols))
        else:
            cols[name] = arr
    return pd.DataFrame(cols)
