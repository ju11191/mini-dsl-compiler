"""Random query generator for differential testing.

Each random query is built as a small tree. From the same tree we render
(1) the tsq DSL text and (2) an independent reference evaluation written
with numpy/pandas (pandas.rolling for windows, pandas.merge_asof for joins).
The reference never touches tsq, so it checks the parser, binder, interpreter
and JIT together.
"""

from __future__ import annotations

import random
from dataclasses import dataclass, field

import numpy as np
import pandas as pd

# Precedence levels (higher binds tighter), mirroring the grammar.
P_OR, P_AND, P_NOT, P_CMP, P_ADD, P_MUL, P_UNARY, P_ATOM = range(8)


@dataclass
class Node:
    text: str
    prec: int
    typ: str  # "i64" | "f64" | "bool"
    ev: object  # callable(frame) -> np.ndarray | numpy scalar
    safe: bool = True  # never NaN/inf (no '/', no NaN-able columns): OK to aggregate


def paren(n: Node, need: int) -> str:
    return f"({n.text})" if n.prec < need else n.text


@dataclass
class Frame:
    cols: dict[str, np.ndarray]
    types: dict[str, str]
    nan_cols: set[str] = field(default_factory=set)  # may contain NaN (asof payload)

    @property
    def n(self) -> int:
        return len(next(iter(self.cols.values()))) if self.cols else 0


@dataclass
class Schema:
    """Column types visible at a point of the pipeline (the generator's view)."""

    types: dict[str, str]
    nan_cols: set[str]


class QueryGen:
    def __init__(self, rng: random.Random, symbols: list[str], ts_lo: int, ts_hi: int):
        self.r = rng
        self.symbols = symbols
        self.ts_lo, self.ts_hi = ts_lo, ts_hi
        self.fresh = 0

    def name(self) -> str:
        self.fresh += 1
        return f"c{self.fresh}"

    # ---------------- expressions ----------------
    def num(self, sch: Schema, depth: int) -> Node:
        r = self.r
        numeric = [c for c, t in sch.types.items() if t in ("i64", "f64") and c != "ts"]
        choice = r.random()
        if depth <= 0 or choice < 0.3:
            if numeric and r.random() < 0.7:
                c = r.choice(numeric)
                return Node(
                    c, P_ATOM, sch.types[c], lambda f, c=c: f.cols[c], c not in sch.nan_cols
                )
            if r.random() < 0.5:
                v = r.randint(0, 200)
                return Node(str(v), P_ATOM, "i64", lambda f, v=v: np.int64(v))
            v = round(r.uniform(0, 200), 2)
            return Node(repr(v), P_ATOM, "f64", lambda f, v=v: np.float64(v))
        if choice < 0.75:
            a, b = self.num(sch, depth - 1), self.num(sch, depth - 1)
            op = r.choice(["+", "-", "*", "/"])
            prec = P_ADD if op in "+-" else P_MUL
            text = f"{paren(a, prec)} {op} {paren(b, prec + 1)}"
            typ = "f64" if op == "/" or "f64" in (a.typ, b.typ) else "i64"
            fn = {
                "+": np.add,
                "-": np.subtract,
                "*": np.multiply,
                "/": np.true_divide,
            }[op]
            safe = a.safe and b.safe and op != "/"
            return Node(text, prec, typ, lambda f, a=a, b=b, fn=fn: fn(a.ev(f), b.ev(f)), safe)
        if choice < 0.85:
            a = self.num(sch, depth - 1)
            return Node(
                f"-{paren(a, P_UNARY)}", P_UNARY, a.typ, lambda f, a=a: np.negative(a.ev(f)), a.safe
            )
        a = self.num(sch, depth - 1)
        if r.random() < 0.5:
            return Node(f"abs({a.text})", P_ATOM, a.typ, lambda f, a=a: np.abs(a.ev(f)), a.safe)
        return Node(
            f"sqrt(abs({a.text}))",
            P_ATOM,
            "f64",
            lambda f, a=a: np.sqrt(np.abs(a.ev(f)).astype(np.float64)),
            a.safe,
        )

    def cmp(self, sch: Schema, depth: int) -> Node:
        r = self.r
        k = r.random()
        if "sym" in sch.types and k < 0.25:
            lit = r.choice(self.symbols + ["ZZZ"])
            code = self.symbols.index(lit) if lit in self.symbols else -1
            op = r.choice(["==", "!="])
            text = f'sym {op} "{lit}"' if r.random() < 0.8 else f'"{lit}" {op} sym'
            fn = np.equal if op == "==" else np.not_equal
            return Node(text, P_CMP, "bool", lambda f, fn=fn, code=code: fn(f.cols["sym"], code))
        if "ts" in sch.types and k < 0.35:
            v = r.randint(self.ts_lo, self.ts_hi)
            op = r.choice(["<", "<=", ">", ">="])
            fn = {"<": np.less, "<=": np.less_equal, ">": np.greater, ">=": np.greater_equal}[op]
            return Node(f"ts {op} {v}", P_CMP, "bool", lambda f, fn=fn, v=v: fn(f.cols["ts"], v))
        a, b = self.num(sch, depth), self.num(sch, depth)
        op = r.choice(["==", "!=", "<", "<=", ">", ">="])
        fn = {
            "==": np.equal,
            "!=": np.not_equal,
            "<": np.less,
            "<=": np.less_equal,
            ">": np.greater,
            ">=": np.greater_equal,
        }[op]
        text = f"{paren(a, P_ADD)} {op} {paren(b, P_ADD)}"
        return Node(text, P_CMP, "bool", lambda f, a=a, b=b, fn=fn: fn(a.ev(f), b.ev(f)))

    def boolean(self, sch: Schema, depth: int) -> Node:
        r = self.r
        k = r.random()
        if depth <= 0 or k < 0.45:
            if r.random() < 0.03:
                v = r.random() < 0.5
                return Node("true" if v else "false", P_ATOM, "bool", lambda f, v=v: np.bool_(v))
            return self.cmp(sch, 1)
        if k < 0.85:
            a, b = self.boolean(sch, depth - 1), self.boolean(sch, depth - 1)
            if r.random() < 0.5:
                text = f"{paren(a, P_AND)} and {paren(b, P_AND + 1)}"
                return Node(
                    text, P_AND, "bool", lambda f, a=a, b=b: np.logical_and(a.ev(f), b.ev(f))
                )
            text = f"{paren(a, P_OR)} or {paren(b, P_OR + 1)}"
            return Node(text, P_OR, "bool", lambda f, a=a, b=b: np.logical_or(a.ev(f), b.ev(f)))
        a = self.boolean(sch, depth - 1)
        return Node(f"not {paren(a, P_NOT)}", P_NOT, "bool", lambda f, a=a: np.logical_not(a.ev(f)))

    # ---------------- stages ----------------
    def stage(self, sch: Schema, joined: bool):
        """Returns (text, apply(frame, data) -> frame, new_schema, joined)."""
        r = self.r
        opts = ["filter", "filter", "derive", "select"]
        if sch.types.get("ts") == "i64":
            opts += ["window", "window"]
            if sch.types.get("sym") == "sym" and not joined:
                opts += ["asof", "asof"]
        kind = r.choice(opts)
        if kind == "filter":
            pred = self.boolean(sch, r.randint(0, 2))

            def apply(f, d, pred=pred):
                m = np.broadcast_to(np.asarray(pred.ev(f), dtype=bool), (f.n,))
                return Frame({c: v[m] for c, v in f.cols.items()}, dict(f.types), set(f.nan_cols))

            return f"filter {pred.text}", apply, sch, joined
        if kind == "derive":
            items = [(self.name(), self.num(sch, r.randint(0, 2))) for _ in range(r.randint(1, 2))]
            types = dict(sch.types)
            nan = set(sch.nan_cols)
            for nm, e in items:
                types[nm] = e.typ
                if not e.safe:
                    nan.add(nm)

            def apply(f, d, items=items, nan=nan):
                cols = dict(f.cols)
                for nm, e in items:
                    cols[nm] = materialize(e.ev(f), f.n, e.typ)
                return Frame(cols, {**f.types, **{nm: e.typ for nm, e in items}}, set(nan))

            text = "derive " + ", ".join(f"{nm} = {e.text}" for nm, e in items)
            return text, apply, Schema(types, nan), joined
        if kind == "select":
            names = list(sch.types)
            keep = [
                c for c in names if r.random() < 0.7 or (c in ("ts", "sym") and r.random() < 0.8)
            ]
            items: list[tuple[str, Node | None]] = [(c, None) for c in keep]
            if r.random() < 0.4 or not items:
                items.append((self.name(), self.num(sch, r.randint(0, 2))))
            r.shuffle(items)
            types, nan = {}, set()
            for nm, e in items:
                types[nm] = sch.types[nm] if e is None else e.typ
                if (e is None and nm in sch.nan_cols) or (e is not None and not e.safe):
                    nan.add(nm)

            def apply(f, d, items=items, nan=nan):
                cols, ty = {}, {}
                for nm, e in items:
                    cols[nm] = f.cols[nm] if e is None else materialize(e.ev(f), f.n, e.typ)
                    ty[nm] = f.types[nm] if e is None else e.typ
                return Frame(cols, ty, set(nan))

            text = "select " + ", ".join(nm if e is None else f"{nm} = {e.text}" for nm, e in items)
            return text, apply, Schema(types, nan), joined
        if kind == "window":
            dur = r.choice([1, 1000, 2_000_000, 50_000_000, 1_000_000_000, 300_000_000_000])
            dur_txt = {
                1: "1ns",
                1000: "1us",
                2_000_000: "2ms",
                50_000_000: "50ms",
                1_000_000_000: "1s",
                300_000_000_000: "5m",
            }[dur]
            cands = [
                c
                for c, t in sch.types.items()
                if t in ("i64", "f64") and c not in sch.nan_cols and c != "ts"
            ]
            aggs = []
            types = dict(sch.types)
            for _ in range(r.randint(1, 3)):
                fn = r.choice(["sum", "mean", "min", "max", "count"] if cands else ["count"])
                col = r.choice(cands) if fn != "count" else None
                default = "count" if fn == "count" else f"{fn}_{col}"
                nm = default if default not in types and r.random() < 0.7 else self.name()
                alias = nm != default
                out_t = "i64" if fn == "count" else ("f64" if fn == "mean" else types[col])
                types[nm] = out_t
                aggs.append((fn, col, nm, alias, out_t))
            txt_aggs = []
            for fn, col, nm, alias, _ in aggs:
                t = f"{fn}({col or ''})"
                txt_aggs.append(f"{t} as {nm}" if alias else t)

            def apply(f, d, aggs=aggs, dur=dur):
                df = pd.DataFrame({"__dt": pd.to_datetime(f.cols["ts"], unit="ns")})
                cols, ty = dict(f.cols), dict(f.types)
                for fn, col, nm, _, out_t in aggs:
                    src = np.ones(f.n) if col is None else f.cols[col].astype(np.float64)
                    df["__x"] = src
                    roll = df.rolling(f"{dur}ns", on="__dt")["__x"]
                    if f.n == 0:
                        vals = np.empty(0)
                    else:
                        vals = {
                            "sum": roll.sum,
                            "mean": roll.mean,
                            "min": roll.min,
                            "max": roll.max,
                            "count": roll.count,
                        }[fn]().to_numpy()
                    cols[nm] = vals  # float64; compared with tolerance
                    ty[nm] = out_t
                return Frame(cols, ty, set(f.nan_cols))

            return (
                f"window {dur_txt} " + ", ".join(txt_aggs),
                apply,
                Schema(types, set(sch.nan_cols)),
                joined,
            )
        # asof
        types = dict(sch.types)
        nan = set(sch.nan_cols)
        payload = []
        for c in ("bid", "ask"):
            nm = c if c not in types else f"{c}_quotes"
            payload.append((c, nm))
            types[nm] = "f64"
            nan.add(nm)

        def apply(f, d, payload=payload):
            left = pd.DataFrame(f.cols)
            q = d["quotes"]
            right = pd.DataFrame(
                {"ts": q["ts"], "sym": q["sym"], **{nm: q[c] for c, nm in payload}}
            )
            if len(left) == 0:
                cols = dict(f.cols)
                for _, nm in payload:
                    cols[nm] = np.empty(0)
            else:
                m = pd.merge_asof(left, right, on="ts", by="sym", direction="backward")
                cols = {c: m[c].to_numpy() for c in list(f.cols) + [nm for _, nm in payload]}
                for c in f.cols:  # merge_asof may upcast; restore original dtypes
                    cols[c] = cols[c].astype(f.cols[c].dtype)
            ty = {**f.types, **{nm: "f64" for _, nm in payload}}
            return Frame(cols, ty, f.nan_cols | {nm for _, nm in payload})

        return "asof_join quotes on sym", apply, Schema(types, nan), True

    def query(self, base_types: dict[str, str], max_stages: int = 4):
        sch = Schema(dict(base_types), set())
        parts, applies = ["trades"], []
        joined = False
        for _ in range(self.r.randint(1, max_stages)):
            text, apply, sch, joined = self.stage(sch, joined)
            parts.append(text)
            applies.append(apply)

        def reference(data):
            f = Frame(dict(data["trades"]), dict(base_types))
            for a in applies:
                f = a(f, data)
            return f

        return " | ".join(parts), reference


_NP = {"i64": np.int64, "f64": np.float64}


def materialize(v, n: int, typ: str) -> np.ndarray:
    arr = np.asarray(v)
    if arr.ndim == 0:
        arr = np.full(n, arr)
    return arr.astype(_NP[typ], copy=False)
