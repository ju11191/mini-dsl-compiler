#include "test_util.h"

#include <gtest/gtest.h>

using namespace tsq;
using namespace tsq::test;

namespace {
constexpr int64_t S = 1000000000LL; // 1 second in ns

struct Fixture : ::testing::Test {
  OwnedTable trades = make_trades({1 * S, 2 * S, 3 * S, 4 * S, 5 * S, 6 * S}, {0, 1, 0, 2, 0, 1},
                                  {99.5, 150.0, 101.0, 100.0, 120.0, 80.0}, {10, 20, 30, 40, 50, 60});
  OwnedTable quotes = make_quotes({0 * S, 2 * S, 2 * S, 5 * S}, {0, 1, 0, 0}, {99.0, 149.0, 100.0, 119.0},
                                  {100.0, 151.0, 102.0, 121.0});
  NamedTables tables() { return {{"trades", named(trades)}, {"quotes", named(quotes)}}; }
  Result run(std::string_view text) { return run_query(text, Backend::Interp, tables()); }
};
} // namespace

TEST_F(Fixture, NoStagesCopiesSource) {
  auto r = run("trades");
  EXPECT_EQ(r.rows, 6);
  EXPECT_EQ(col_values<double>(r, "price"), (std::vector<double>{99.5, 150.0, 101.0, 100.0, 120.0, 80.0}));
}

TEST_F(Fixture, FilterAndSym) {
  auto r = run(R"(trades | filter price > 100 and sym == "AAPL")");
  EXPECT_EQ(col_values<int64_t>(r, "ts"), (std::vector<int64_t>{3 * S, 5 * S}));
  EXPECT_EQ(col_values<int64_t>(r, "size"), (std::vector<int64_t>{30, 50}));
  EXPECT_EQ(col_values<int32_t>(r, "sym"), (std::vector<int32_t>{0, 0}));
}

TEST_F(Fixture, FilterOrNot) {
  auto r = run(R"(trades | filter not (sym == "AAPL") or size >= 50)");
  EXPECT_EQ(col_values<int64_t>(r, "size"), (std::vector<int64_t>{20, 40, 50, 60}));
}

TEST_F(Fixture, UnknownSymbolMatchesNothing) {
  EXPECT_EQ(run(R"(trades | filter sym == "ZZZZ")").rows, 0);
  EXPECT_EQ(run(R"(trades | filter sym != "ZZZZ")").rows, 6);
}

TEST_F(Fixture, SelectDerive) {
  auto r = run("trades | filter size <= 20 | select ts, n = price * size, h = size / 4, a = abs(10 - size), "
               "q = sqrt(size - 1), m = -size | derive k = n + 1");
  EXPECT_EQ(r.cols.size(), 7u);
  EXPECT_EQ(col_values<double>(r, "n"), (std::vector<double>{995.0, 3000.0}));
  EXPECT_EQ(col_values<double>(r, "h"), (std::vector<double>{2.5, 5.0}));
  EXPECT_EQ(col_values<int64_t>(r, "a"), (std::vector<int64_t>{0, 10}));
  EXPECT_EQ(col_values<double>(r, "q"), (std::vector<double>{3.0, std::sqrt(19.0)}));
  EXPECT_EQ(col_values<int64_t>(r, "m"), (std::vector<int64_t>{-10, -20}));
  EXPECT_EQ(col_values<double>(r, "k"), (std::vector<double>{996.0, 3001.0}));
}

TEST_F(Fixture, IntegerArithmeticWraps) {
  auto r = run("trades | filter size == 10 | select x = 9223372036854775807 + size, y = size * 1000000000000000000");
  EXPECT_EQ(col_values<int64_t>(r, "x")[0], int64_t(0x8000000000000009ULL));
  EXPECT_EQ(col_values<int64_t>(r, "y")[0], int64_t(uint64_t(10) * 1000000000000000000ULL));
}

TEST_F(Fixture, WindowAggregates) {
  // window 2s: row k covers ts in (ts_k - 2s, ts_k] => rows k-1 and k.
  auto r = run("trades | window 2s sum(size), mean(price), min(price), max(price), count(), sum(price) as sp, "
               "min(size) as mns, mean(size) as ms");
  EXPECT_EQ(col_values<int64_t>(r, "sum_size"), (std::vector<int64_t>{10, 30, 50, 70, 90, 110}));
  EXPECT_EQ(col_values<double>(r, "mean_price"),
            (std::vector<double>{99.5, (99.5 + 150.0) / 2, (150.0 + 101.0) / 2, (101.0 + 100.0) / 2,
                                 (100.0 + 120.0) / 2, (120.0 + 80.0) / 2}));
  EXPECT_EQ(col_values<double>(r, "min_price"), (std::vector<double>{99.5, 99.5, 101.0, 100.0, 100.0, 80.0}));
  EXPECT_EQ(col_values<double>(r, "max_price"), (std::vector<double>{99.5, 150.0, 150.0, 101.0, 120.0, 120.0}));
  EXPECT_EQ(col_values<int64_t>(r, "count"), (std::vector<int64_t>{1, 2, 2, 2, 2, 2}));
  EXPECT_EQ(col_values<int64_t>(r, "mns"), (std::vector<int64_t>{10, 10, 20, 30, 40, 50}));
  EXPECT_EQ(col_values<double>(r, "ms"), (std::vector<double>{10, 15, 25, 35, 45, 55}));
}

TEST_F(Fixture, WindowAfterFilterUsesFilteredRows) {
  // AAPL rows at 1s, 3s, 5s; window 3s => (t-3s, t]
  auto r = run(R"(trades | filter sym == "AAPL" | window 3s sum(size), count())");
  EXPECT_EQ(col_values<int64_t>(r, "sum_size"), (std::vector<int64_t>{10, 40, 80}));
  EXPECT_EQ(col_values<int64_t>(r, "count"), (std::vector<int64_t>{1, 2, 2}));
}

TEST_F(Fixture, WindowDuplicateTimestamps) {
  auto t = make_trades({1, 1, 1, 5, 5}, {0, 0, 0, 0, 0}, {3, 1, 2, 9, 0}, {1, 2, 3, 4, 5});
  NamedTables nt{{"trades", named(t)}};
  auto r = run_query("trades | window 2ns min(price), sum(size), count()", Backend::Interp, nt);
  // Rows with equal ts: each row sees earlier rows (by position) with the same ts, not later ones.
  EXPECT_EQ(col_values<double>(r, "min_price"), (std::vector<double>{3, 1, 1, 9, 0}));
  EXPECT_EQ(col_values<int64_t>(r, "sum_size"), (std::vector<int64_t>{1, 3, 6, 4, 9}));
  EXPECT_EQ(col_values<int64_t>(r, "count"), (std::vector<int64_t>{1, 2, 3, 1, 2}));
}

TEST_F(Fixture, AsofJoin) {
  auto r = run("trades | asof_join quotes on sym");
  auto bid = col_values<double>(r, "bid");
  auto ask = col_values<double>(r, "ask");
  // t=1 AAPL -> quote@0 (99); t=2 MSFT -> quote@2 MSFT (149); t=3 AAPL -> last AAPL <=3 is @2 (100);
  // t=4 GOOG -> none (NaN); t=5 AAPL -> @5 (119); t=6 MSFT -> @2 (149)
  std::vector<double> eb = {99.0, 149.0, 100.0, NAN, 119.0, 149.0};
  ASSERT_EQ(bid.size(), eb.size());
  for (size_t i = 0; i < eb.size(); ++i) {
    if (std::isnan(eb[i])) EXPECT_TRUE(std::isnan(bid[i])) << i;
    else EXPECT_EQ(bid[i], eb[i]) << i;
  }
  EXPECT_EQ(ask[0], 100.0);
  EXPECT_EQ(ask[2], 102.0);
}

TEST_F(Fixture, FullPipeline) {
  auto r = run(R"(trades | filter price > 100 and sym == "AAPL" | window 5m mean(price) | asof_join quotes on sym)");
  EXPECT_EQ(r.rows, 2);
  EXPECT_EQ(col_values<double>(r, "mean_price"), (std::vector<double>{101.0, 110.5}));
  EXPECT_EQ(col_values<double>(r, "bid"), (std::vector<double>{100.0, 119.0}));
}

TEST_F(Fixture, EmptyInput) {
  auto t = make_trades({}, {}, {}, {});
  auto q = make_quotes({}, {}, {}, {});
  NamedTables nt{{"trades", named(t)}, {"quotes", named(q)}};
  auto r = run_query("trades | filter price > 1 | window 1s mean(price) | asof_join quotes on sym", Backend::Interp, nt);
  EXPECT_EQ(r.rows, 0);
  EXPECT_EQ(r.cols.size(), 7u);
}

TEST_F(Fixture, InputValidation) {
  auto bad = make_trades({2, 1}, {0, 0}, {1, 1}, {1, 1});
  NamedTables nt{{"trades", named(bad)}};
  EXPECT_THROW(run_query("trades | window 1s count()", Backend::Interp, nt), RuntimeError);
  EXPECT_NO_THROW(run_query("trades | filter size > 0", Backend::Interp, nt)); // no ts requirement
  auto badsym = make_trades({1, 2}, {0, 7}, {1, 1}, {1, 1});
  NamedTables nt2{{"trades", named(badsym)}};
  EXPECT_THROW(run_query("trades", Backend::Interp, nt2), RuntimeError);
  NamedTables missing{};
  EXPECT_THROW(run_query("trades", Backend::Interp, missing), RuntimeError);
}
