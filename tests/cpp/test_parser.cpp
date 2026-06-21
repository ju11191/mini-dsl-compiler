#include "parser.h"

#include <gtest/gtest.h>

using namespace tsq;

static std::string roundtrip(std::string_view s) { return to_string(parse(s)); }

TEST(Parser, ExampleQuery) {
  auto q = parse(R"(trades | filter price > 100 and sym == "AAPL" | window 5m mean(price) | asof_join quotes on sym)");
  EXPECT_EQ(q.source, "trades");
  ASSERT_EQ(q.stages.size(), 3u);
  EXPECT_EQ(q.stages[0].kind, Stage::Kind::Filter);
  EXPECT_EQ(to_string(*q.stages[0].pred), "((price > 100) and (sym == \"AAPL\"))");
  EXPECT_EQ(q.stages[1].kind, Stage::Kind::Window);
  EXPECT_EQ(q.stages[1].dur_ns, 300000000000LL);
  ASSERT_EQ(q.stages[1].aggs.size(), 1u);
  EXPECT_EQ(q.stages[1].aggs[0].fn, AggFn::Mean);
  EXPECT_EQ(q.stages[1].aggs[0].col, "price");
  EXPECT_EQ(q.stages[2].kind, Stage::Kind::AsofJoin);
  EXPECT_EQ(q.stages[2].table, "quotes");
  EXPECT_EQ(q.stages[2].key, "sym");
}

TEST(Parser, Precedence) {
  EXPECT_EQ(roundtrip("t | filter a + b * c > d - e / f"), "t | filter ((a + (b * c)) > (d - (e / f)))");
  EXPECT_EQ(roundtrip("t | filter a or b and not c"), "t | filter (a or (b and (not c)))");
  EXPECT_EQ(roundtrip("t | filter not a == b"), "t | filter (not (a == b))");
  EXPECT_EQ(roundtrip("t | filter -a * -b < 0"), "t | filter (((-a) * (-b)) < 0)");
  EXPECT_EQ(roundtrip("t | filter (a or b) and c"), "t | filter ((a or b) and c)");
  EXPECT_EQ(roundtrip("t | filter a - b - c > 0"), "t | filter (((a - b) - c) > 0)");
}

TEST(Parser, SelectDeriveWindow) {
  EXPECT_EQ(roundtrip("t | select ts, n = price * size | derive x = abs(n), y = 1.5"),
            "t | select ts, n = (price * size) | derive x = abs(n), y = 1.5");
  EXPECT_EQ(roundtrip("t | window 30s sum(size) as vol, min(price), count()"),
            "t | window 30000000000ns sum(size) as vol, min(price), count()");
}

TEST(Parser, MultilineAndComments) {
  EXPECT_EQ(roundtrip("trades   # source\n  | filter price > 1  # big\n  | select ts\n"),
            "trades | filter (price > 1) | select ts");
}

TEST(Parser, ExprLocationSpansWholeExpression) {
  auto q = parse("t | filter price > 100");
  EXPECT_EQ(q.stages[0].pred->loc.offset, 11u);
  EXPECT_EQ(q.stages[0].pred->loc.len, 11u);
}

struct ErrCase {
  const char* src;
  const char* msg;
  uint32_t col;
};

class ParserErrors : public ::testing::TestWithParam<ErrCase> {};

TEST_P(ParserErrors, MessageAndLocation) {
  const auto& c = GetParam();
  try {
    parse(c.src);
    FAIL() << "expected error for: " << c.src;
  } catch (const CompileError& e) {
    EXPECT_EQ(e.message(), c.msg) << c.src;
    EXPECT_EQ(e.loc().col, c.col) << c.src;
  }
}

INSTANTIATE_TEST_SUITE_P(
    All, ParserErrors,
    ::testing::Values(
        ErrCase{"", "expected a table name at the start of the query, found end of query", 1},
        ErrCase{"t | fliter x > 1", "unknown stage 'fliter' (did you mean 'filter'?); stages are filter, select, derive, window, asof_join", 5},
        ErrCase{"t filter x", "expected '|' before stage 'filter'", 3},
        ErrCase{"t | filter", "'filter' needs a predicate", 5},
        ErrCase{"t | filter x >", "expected an expression, found end of query", 15},
        ErrCase{"t | filter (x > 1", "expected ')', found end of query", 18},
        ErrCase{"t | filter x = 1", "'=' is assignment; use '==' to compare", 14},
        ErrCase{"t | filter a < b < c", "comparisons cannot be chained; use 'and' (e.g. a < b and b < c)", 18},
        ErrCase{"t | window mean(x)", "expected a window duration such as 5m or 30s after 'window', found identifier 'mean'", 12},
        ErrCase{"t | window 5m men(x)", "unknown aggregate 'men' (did you mean 'mean'?); aggregates are sum, mean, min, max, count", 15},
        ErrCase{"t | window 5m mean()", "expected a column name inside mean(...), found ')'", 20},
        ErrCase{"t | asof_join q sym", "expected 'on <key>' after 'asof_join q', found identifier 'sym'", 17},
        ErrCase{"t | derive x", "expected '=' after 'x' in derive (derive name = expr)", 13},
        ErrCase{"t | select a + b", "computed select items need a name: write 'name = expr'", 14},
        ErrCase{"t | select a,", "expected a column name or 'name = expr', found end of query", 14},
        ErrCase{"t | 5", "expected a stage name after '|', found '5'", 5}));
