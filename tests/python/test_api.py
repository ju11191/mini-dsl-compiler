import numpy as np
import pytest
import tsq

EXAMPLE = (
    'trades | filter price > 100 and sym == "AAPL" | window 5m mean(price) '
    "| asof_join quotes on sym"
)


def test_compile_and_run(small_data):
    q = tsq.compile(EXAMPLE)
    out = q.run(small_data.tables, small_data.symbols)
    assert list(out) == ["ts", "sym", "price", "size", "mean_price", "bid", "ask"]
    n = len(out["ts"])
    assert 0 < n < len(small_data.tables["trades"]["ts"])
    assert all(len(v) == n for v in out.values())
    assert out["sym"].dtype == np.int32 and out["price"].dtype == np.float64
    assert (out["price"] > 100).all()
    aapl = small_data.symbols.index("AAPL")
    assert (out["sym"] == aapl).all()
    assert q.output_schema[-1] == ("ask", "f64")
    assert set(q.compile_stats) == {
        "parse_bind_ms",
        "irgen_ms",
        "optimize_ms",
        "codegen_ms",
        "total_ms",
    }


def test_interp_and_jit_agree(small_data):
    a = tsq.compile(EXAMPLE, backend="jit").run(small_data.tables, small_data.symbols)
    b = tsq.compile(EXAMPLE, backend="interp").run(small_data.tables, small_data.symbols)
    for k in a:
        np.testing.assert_array_equal(a[k], b[k])


def test_explicit_schema_compiles_eagerly():
    schema = {"trades": [("ts", "i64"), ("sym", "sym"), ("price", "f64"), ("size", "i64")]}
    q = tsq.compile("trades | select ts, n = price * size", schema=schema)
    assert "@tsq_query(" in q.ir()
    assert "define i64 @tsq_query(" in q.ir(optimized=False)


def test_schema_cache(small_data):
    q = tsq.compile("trades | filter size > 500")
    q.run(small_data.tables, small_data.symbols)
    q.run(small_data.tables, small_data.symbols)
    assert len(q._cache) == 1
    # A different dtype for `size` is a different schema -> recompiles.
    t = dict(small_data.tables["trades"])
    t["size"] = t["size"].astype(np.float64)
    out = q.run({"trades": t}, small_data.symbols)
    assert len(q._cache) == 2
    assert out["size"].dtype == np.float64


def test_single_table_shortcut():
    cols = {"ts": np.arange(5, dtype=np.int64), "x": np.array([1.0, 5, 2, 8, 3])}
    out = tsq.compile("t | filter x > 2 | window 2ns max(x)").run(cols)
    np.testing.assert_array_equal(out["x"], [5, 8, 3])
    np.testing.assert_array_equal(out["max_x"], [5, 8, 8])


def test_non_contiguous_input_is_handled():
    base = np.arange(20, dtype=np.int64)
    cols = {"ts": base[::2], "v": np.arange(10, dtype=np.int64)}
    out = tsq.compile("t | filter v >= 8").run(cols)
    np.testing.assert_array_equal(out["ts"], [16, 18])


def test_syntax_error_raised_at_compile():
    with pytest.raises(tsq.CompileError) as ei:
        tsq.compile("trades | filter price >")
    e = ei.value
    assert e.line == 1 and e.col == 24
    assert "expected an expression" in e.message
    assert "^" in str(e)


def test_type_error_with_location(small_data):
    q = tsq.compile('trades | filter sym > "A"')  # parses fine; binding needs a schema
    with pytest.raises(tsq.CompileError) as ei:
        q.run(small_data.tables, small_data.symbols)
    assert "sym values only support == and !=" in ei.value.message
    assert ei.value.col == 17
    assert isinstance(ei.value, ValueError)


def test_data_errors(small_data):
    t = {k: v.copy() for k, v in small_data.tables["trades"].items()}
    t["ts"][[0, 1]] = t["ts"][[1, 0]] + np.array([0, 1])  # make ts decrease
    t["ts"][0] = t["ts"][1] + 5
    with pytest.raises(tsq.DataError, match="ts must be ascending"):
        tsq.compile("trades | window 1s count()").run({"trades": t}, small_data.symbols)
    # validate=False skips the check (garbage in, garbage out, but memory-safe)
    tsq.compile("trades | window 1s count()").run({"trades": t}, small_data.symbols, validate=False)
    with pytest.raises(tsq.DataError, match="outside the symbol dictionary"):
        tsq.compile("trades").run(small_data.tables, ["AAPL"])
    with pytest.raises(KeyError):
        tsq.compile("trades | asof_join quotes on sym").run(
            {"trades": small_data.tables["trades"]}, small_data.symbols
        )
    with pytest.raises(TypeError, match="dtype"):
        tsq.compile("t").run({"t": {"x": np.zeros(3, dtype=np.float32)}})
    with pytest.raises(ValueError, match="rows"):
        tsq.compile("t").run({"t": {"x": np.zeros(3), "y": np.zeros(4)}})


def test_unknown_symbol_literal_matches_nothing(small_data):
    out = tsq.compile('trades | filter sym == "ZZZ"').run(small_data.tables, small_data.symbols)
    assert len(out["ts"]) == 0


def test_result_owns_memory(small_data):
    out = tsq.compile("trades | select ts").run(small_data.tables, small_data.symbols)
    ts = out["ts"]
    del out
    assert ts.sum() != 0  # still valid after the dict is gone


def test_to_pandas(small_data):
    out = tsq.compile("trades | filter size > 990").run(small_data.tables, small_data.symbols)
    df = tsq.to_pandas(out, small_data.symbols)
    assert set(df["sym"].astype(str)) <= set(small_data.symbols)
    assert len(df) == len(out["ts"])


def test_datagen_shapes():
    d = tsq.datagen.make_dataset(10_000, 5_000, seed=3)
    tr, qu = d.tables["trades"], d.tables["quotes"]
    assert len(tr["ts"]) == 10_000 and len(qu["ts"]) == 5_000
    assert (np.diff(tr["ts"]) >= 0).all() and (np.diff(qu["ts"]) >= 0).all()
    assert tr["sym"].min() >= 0 and tr["sym"].max() < len(d.symbols)
    assert (tr["size"] >= 1).all() and (tr["size"] <= 1000).all()
    assert (qu["ask"] > qu["bid"]).all()
    aapl = tr["price"][tr["sym"] == 0]
    assert 90 < aapl.mean() < 110
    # deterministic
    d2 = tsq.datagen.make_dataset(10_000, 5_000, seed=3)
    np.testing.assert_array_equal(d2.tables["trades"]["price"], tr["price"])


def test_concurrent_runs_of_one_query(small_data):
    from concurrent.futures import ThreadPoolExecutor

    q = tsq.compile("trades | window 50ms max(price), sum(size) | asof_join quotes on sym")
    expected = q.run(small_data.tables, small_data.symbols)
    with ThreadPoolExecutor(4) as ex:
        outs = list(ex.map(lambda _: q.run(small_data.tables, small_data.symbols), range(16)))
    for out in outs:
        for k in expected:
            np.testing.assert_array_equal(out[k], expected[k])
