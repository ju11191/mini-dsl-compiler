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

TEST(Jit, IrIsOneFusedLoop) {
  auto q = CompiledQuery::compile(R"(trades | filter price > 100 and sym == "AAPL" | select ts, n = price * size)",
                                  trades_quotes_catalog(), Backend::Jit);
  const std::string& ir = q->ir_optimized();
  EXPECT_NE(ir.find("define i64 @tsq_query"), std::string::npos);
  EXPECT_EQ(ir.find("call "), std::string::npos) << "fused loop should not call anything:\n" << ir;
  EXPECT_FALSE(q->ir_unoptimized().empty());
  EXPECT_GT(q->stats().codegen_ms, 0.0);
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
  NamedTables nt{{"trades", named(t)}};
  EXPECT_EQ(run_query("trades | filter price > 1 | select ts", Backend::Jit, nt).rows, 0);
}
