// JIT vs interpreter on fixed queries: results must be bit-identical.
#include "datagen.h"
#include "test_util.h"

#include <gtest/gtest.h>

using namespace tsq;
using namespace tsq::test;

namespace {

struct Data {
  OwnedTable trades = gen_trades(20000, 42, 3);
  OwnedTable quotes = gen_quotes(15000, 43, 3);
  NamedTables tables() const { return {{"trades", named(trades)}, {"quotes", named(quotes)}}; }
};

const Data& data() {
  static Data d;
  return d;
}

void expect_same(std::string_view text, const CompileOptions& opts = {}) {
  auto cat = trades_quotes_catalog();
  auto qi = CompiledQuery::compile(std::string(text), cat, Backend::Interp);
  auto qj = CompiledQuery::compile(std::string(text), cat, Backend::Jit, opts);
  auto ri = qi->run(data().tables(), syms());
  auto rj = qj->run(data().tables(), syms());
  EXPECT_TRUE(results_identical(ri, rj)) << text;
  // Running the same compiled query again (scratch reuse) gives the same answer.
  auto rj2 = qj->run(data().tables(), syms());
  EXPECT_TRUE(results_identical(rj, rj2)) << text;
}

} // namespace

class JitVsInterp : public ::testing::TestWithParam<const char*> {};

TEST_P(JitVsInterp, Identical) {
  expect_same(GetParam());
  CompileOptions o0;
  o0.opt_level = 0;
  expect_same(GetParam(), o0);
  CompileOptions copy;
  copy.window_direct_input = false;
  expect_same(GetParam(), copy);
  copy.opt_level = 0;
  expect_same(GetParam(), copy);
}

INSTANTIATE_TEST_SUITE_P(
    FilterProject, JitVsInterp,
    ::testing::Values(
        "trades",
        "trades | filter price > 100",
        R"(trades | filter price > 100 and sym == "AAPL")",
        R"(trades | filter sym != "MSFT" or size < 10)",
        R"(trades | filter sym == "NOPE")",
        "trades | filter not (size > 500) and price <= 100.5",
        "trades | filter size == 7 or size != size",
        "trades | filter false",
        "trades | filter true and price * size / 3 > 20000",
        "trades | select ts, notional = price * size",
        "trades | select a = size - 500, b = -price, c = abs(size - 500), d = abs(-price), e = sqrt(price)",
        "trades | derive x = size * 3 + 1, y = size / 7, z = ts - 1704205800000000000 | filter x > 1000 | select z, y",
        "trades | filter price > 100 | derive n = price * size | filter n > 50000 | select ts, sym, n",
        "trades | select sym, ts | filter sym == \"GOOG\"",
        "trades | derive big = 9223372036854775807 + size, m = size * 4611686018427387904"));

INSTANTIATE_TEST_SUITE_P(
    Window, JitVsInterp,
    ::testing::Values(
        "trades | window 5m mean(price)",
        "trades | window 10ms sum(size), sum(price), mean(size), mean(price), min(price), max(price), min(size), "
        "max(size), count()",
        "trades | window 1ns count(), max(price)",
        "trades | window 1d sum(size) as total, min(price) as lo",
        R"(trades | filter sym == "AAPL" | window 30s mean(price), max(price), min(price))",
        "trades | filter size > 900 | window 2s sum(size) | filter sum_size > 2000",
        "trades | derive n = price * size | window 50ms max(n), mean(n) | select ts, max_n, mean_n",
        "trades | window 1s mean(price) | window 5s max(mean_price) as m2 | filter m2 > 100",
        "trades | window 5ms min(price) | filter price > 100 | window 7ms min(price) as m2"));

INSTANTIATE_TEST_SUITE_P(
    Asof, JitVsInterp,
    ::testing::Values(
        "trades | asof_join quotes on sym",
        R"(trades | filter sym == "MSFT" | asof_join quotes on sym | derive spread = ask - bid)",
        "trades | asof_join quotes on sym | filter price > ask or price < bid",
        "trades | window 5m mean(price) | asof_join quotes on sym | select ts, mean_price, bid",
        R"(trades | filter price > 100 and sym == "AAPL" | window 5m mean(price) | asof_join quotes on sym)",
        "trades | asof_join quotes on sym | window 1s max(ask)"));

TEST(Jit, IrIsOneFusedLoop) {
  auto q = CompiledQuery::compile(R"(trades | filter price > 100 and sym == "AAPL" | select ts, n = price * size)",
                                  trades_quotes_catalog(), Backend::Jit);
  const std::string& ir = q->ir_optimized();
  EXPECT_NE(ir.find("define i64 @tsq_query"), std::string::npos);
  EXPECT_EQ(ir.find("call "), std::string::npos) << "fused loop should not call anything:\n" << ir;
  EXPECT_FALSE(q->ir_unoptimized().empty());
  EXPECT_GT(q->stats().codegen_ms, 0.0);
}

TEST(Jit, WindowDirectInputSkipsHistoryCopy) {
  auto cat = trades_quotes_catalog();
  CompileOptions copy, direct;
  copy.window_direct_input = false;
  direct.window_direct_input = true;
  const char* text = "trades | window 5m sum(size), max(price)";
  auto a = CompiledQuery::compile(text, cat, Backend::Jit, copy);
  auto b = CompiledQuery::compile(text, cat, Backend::Jit, direct);
  EXPECT_NE(a->ir_unoptimized().find("win.hist.ts"), std::string::npos);
  EXPECT_EQ(b->ir_unoptimized().find("win.hist."), std::string::npos) << b->ir_unoptimized();
  // After a filter the window input is no longer the source rows: history is copied.
  auto c = CompiledQuery::compile("trades | filter size > 5 | window 5m max(price)", cat, Backend::Jit, direct);
  EXPECT_NE(c->ir_unoptimized().find("win.hist.ts"), std::string::npos);
}

TEST(Jit, RunRawIntoCallerBuffers) {
  auto q = CompiledQuery::compile("trades | filter size > 990 | select ts, size", trades_quotes_catalog(), Backend::Jit);
  auto bi = q->bind_inputs(data().tables(), syms());
  std::vector<int64_t> ts(size_t(data().trades.rows)), size(size_t(data().trades.rows));
  void* outs[] = {ts.data(), size.data()};
  int64_t n = q->run_raw(bi, outs);
  auto r = q->run(bi);
  ASSERT_EQ(n, r.rows);
  for (int64_t i = 0; i < n; ++i) {
    EXPECT_EQ(ts[i], r.col("ts").as<int64_t>()[i]);
    EXPECT_GT(size[i], 990);
  }
}

TEST(Jit, EmptyInput) {
  auto t = make_trades({}, {}, {}, {});
  auto q = make_quotes({}, {}, {}, {});
  NamedTables nt{{"trades", named(t)}, {"quotes", named(q)}};
  EXPECT_EQ(run_query("trades | filter price > 1 | select ts", Backend::Jit, nt).rows, 0);
  EXPECT_EQ(run_query("trades | window 1s max(price) | asof_join quotes on sym", Backend::Jit, nt).rows, 0);
}

TEST(Jit, AsofEmptyRightSideGivesNaN) {
  auto t = make_trades({1, 2}, {0, 1}, {1, 2}, {1, 2});
  auto q = make_quotes({}, {}, {}, {});
  NamedTables nt{{"trades", named(t)}, {"quotes", named(q)}};
  auto r = run_query("trades | asof_join quotes on sym", Backend::Jit, nt);
  ASSERT_EQ(r.rows, 2);
  EXPECT_TRUE(std::isnan(r.col("bid").as<double>()[0]));
  EXPECT_TRUE(std::isnan(r.col("ask").as<double>()[1]));
}

TEST(Jit, HandComputedWindowAndAsof) {
  constexpr int64_t S = 1000000000LL;
  auto t = make_trades({1 * S, 2 * S, 3 * S, 4 * S, 5 * S, 6 * S}, {0, 1, 0, 2, 0, 1},
                       {99.5, 150.0, 101.0, 100.0, 120.0, 80.0}, {10, 20, 30, 40, 50, 60});
  auto q = make_quotes({0 * S, 2 * S, 2 * S, 5 * S}, {0, 1, 0, 0}, {99.0, 149.0, 100.0, 119.0},
                       {100.0, 151.0, 102.0, 121.0});
  NamedTables nt{{"trades", named(t)}, {"quotes", named(q)}};
  auto r = run_query(R"(trades | filter price > 100 and sym == "AAPL" | window 5m mean(price) | asof_join quotes on sym)",
                     Backend::Jit, nt);
  EXPECT_EQ(col_values<double>(r, "mean_price"), (std::vector<double>{101.0, 110.5}));
  EXPECT_EQ(col_values<double>(r, "bid"), (std::vector<double>{100.0, 119.0}));
  auto w = run_query("trades | window 2s min(price), max(price), sum(size)", Backend::Jit, nt);
  EXPECT_EQ(col_values<double>(w, "min_price"), (std::vector<double>{99.5, 99.5, 101.0, 100.0, 100.0, 80.0}));
  EXPECT_EQ(col_values<double>(w, "max_price"), (std::vector<double>{99.5, 150.0, 150.0, 101.0, 120.0, 120.0}));
  EXPECT_EQ(col_values<int64_t>(w, "sum_size"), (std::vector<int64_t>{10, 30, 50, 70, 90, 110}));
}
