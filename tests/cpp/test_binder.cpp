#include "test_util.h"

#include <gtest/gtest.h>

using namespace tsq;
using namespace tsq::test;

TEST(Binder, FilterTypesAndPromotion) {
  auto q = bind_text("trades | filter price > 100 and size * 2 < 7.5");
  ASSERT_EQ(q.stages.size(), 1u);
  EXPECT_EQ(to_string(*q.stages[0].pred, q.input(), q),
            "((price > 100f):bool and (f64((size * 2):i64) < 7.5f):bool):bool");
  EXPECT_EQ(q.output, q.input());
}

TEST(Binder, SymLiteralsBecomeParams) {
  auto q = bind_text(R"(trades | filter sym == "AAPL" or "MSFT" == sym or sym != "AAPL")");
  ASSERT_EQ(q.sym_literals.size(), 2u);
  EXPECT_EQ(q.sym_literals[0], "AAPL");
  EXPECT_EQ(q.sym_literals[1], "MSFT");
}

TEST(Binder, DivisionIsFloat) {
  auto q = bind_text("trades | derive r = size / 2");
  EXPECT_EQ(q.output.back(), (ColumnDef{"r", Type::F64}));
}

TEST(Binder, SelectAndDeriveSchemas) {
  auto q = bind_text("trades | select ts, n = price * size | derive k = n + 1, m = -ts");
  EXPECT_EQ(schema_to_string(q.stages[0].out), "ts:i64, n:f64");
  EXPECT_EQ(schema_to_string(q.output), "ts:i64, n:f64, k:f64, m:i64");
}

TEST(Binder, WindowSchema) {
  auto q = bind_text("trades | window 5m mean(price), sum(size), max(price) as hi, count()");
  EXPECT_EQ(
      schema_to_string(q.output),
      "ts:i64, sym:sym, price:f64, size:i64, mean_price:f64, sum_size:i64, hi:f64, count:i64");
  EXPECT_EQ(q.stages[0].ts_col, 0);
  EXPECT_TRUE(q.tables[0].needs_sorted_ts);
}

TEST(Binder, AsofSchemaAndRename) {
  Catalog c = trades_quotes_catalog();
  c.tables["q2"] = {
      {"ts", Type::I64}, {"sym", Type::Sym}, {"price", Type::F64}, {"lvl", Type::I64}};
  auto q = bind_text("trades | asof_join quotes on sym | asof_join q2 on sym", c);
  EXPECT_EQ(schema_to_string(q.output),
            "ts:i64, sym:sym, price:f64, size:i64, bid:f64, ask:f64, price_q2:f64, lvl:f64");
  ASSERT_EQ(q.tables.size(), 3u);
  EXPECT_EQ(q.tables[1].name, "quotes");
  EXPECT_TRUE(q.tables[1].needs_sorted_ts);
}

struct ErrCase {
  const char* src;
  const char* msg;
  uint32_t col;
};

class BinderErrors : public ::testing::TestWithParam<ErrCase> {};

TEST_P(BinderErrors, MessageAndLocation) {
  const auto& c = GetParam();
  try {
    bind_text(c.src);
    FAIL() << "expected error for: " << c.src;
  } catch (const CompileError& e) {
    EXPECT_EQ(e.message(), c.msg) << c.src;
    EXPECT_EQ(e.loc().col, c.col) << c.src;
  }
}

INSTANTIATE_TEST_SUITE_P(
    All, BinderErrors,
    ::testing::Values(
        ErrCase{"trade | filter price > 1",
                "unknown table 'trade' (did you mean 'trades'?); available tables: quotes, trades",
                1},
        ErrCase{"trades | filter prise > 1",
                "unknown column 'prise' (did you mean 'price'?); available columns: ts, sym, "
                "price, size",
                17},
        ErrCase{"trades | filter price",
                "filter predicate must be bool, but this expression is f64", 17},
        ErrCase{"trades | filter sym > \"A\"", "sym values only support == and !=, not '>'", 17},
        ErrCase{"trades | filter price == \"A\"", "cannot compare f64 with a string literal", 17},
        ErrCase{"trades | filter sym == 3",
                "operator '==' cannot compare these types, found sym and i64", 17},
        ErrCase{"trades | filter price and size > 1",
                "operator 'and' needs bool operands, found f64 and bool", 17},
        ErrCase{"trades | filter not price", "'not' needs a bool operand, found f64", 17},
        ErrCase{"trades | derive x = sym + 1",
                "operator '+' needs numeric operands, found sym and i64", 21},
        ErrCase{"trades | derive x = foo(price)", "unknown function 'foo'; functions are abs, sqrt",
                21},
        ErrCase{"trades | derive x = mean(price)",
                "unknown function 'mean' ('mean' is an aggregate; use it in a window stage); "
                "functions are abs, sqrt",
                21},
        ErrCase{"trades | derive price = 1",
                "column 'price' already exists; derive only adds new columns (use select to "
                "replace it)",
                17},
        ErrCase{"trades | select a = price, a = size", "duplicate output column 'a'", 28},
        ErrCase{"trades | derive f = price > 1",
                "column 'f' would have type bool, which cannot be stored; use the condition in a "
                "filter",
                21},
        ErrCase{"trades | select price | window 5m mean(price)",
                "window needs an i64 'ts' column, but the input to this stage has none (was it "
                "dropped by an earlier select?)",
                25},
        ErrCase{"trades | window 5m sum(sym)", "sum() needs a numeric column, but 'sym' is sym",
                24},
        ErrCase{"trades | window 5m mean(price), mean(price)",
                "window output column 'mean_price' already exists; rename it with 'as'", 33},
        ErrCase{"trades | asof_join quote on sym",
                "unknown table 'quote' (did you mean 'quotes'?); available tables: quotes, trades",
                20},
        ErrCase{"trades | asof_join quotes on size",
                "join key 'size' is not a column of table 'quotes'; available columns: ts, sym, "
                "bid, ask",
                30},
        ErrCase{"trades | asof_join quotes on ts",
                "asof_join key must be a sym column on both sides (left is i64, right is i64)", 30},
        ErrCase{"trades | filter \"a\" == \"b\"", "cannot compare two string literals", 17},
        ErrCase{"trades | derive x = abs(price, 1)", "abs() takes exactly 1 argument", 21}));
