#pragma once

#include "binder.h"
#include "parser.h"

#include <gtest/gtest.h>

namespace tsq::test {

inline Catalog trades_quotes_catalog() {
  Catalog c;
  c.tables["trades"] = {{"ts", Type::I64}, {"sym", Type::Sym}, {"price", Type::F64}, {"size", Type::I64}};
  c.tables["quotes"] = {{"ts", Type::I64}, {"sym", Type::Sym}, {"bid", Type::F64}, {"ask", Type::F64}};
  return c;
}

inline BoundQuery bind_text(std::string_view text, const Catalog& c = trades_quotes_catalog()) {
  return bind(parse(text), c);
}

} // namespace tsq::test

#include "engine.h"

#include <cmath>
#include <initializer_list>

namespace tsq::test {

inline std::vector<std::string> syms() { return {"AAPL", "MSFT", "GOOG"}; }

// Small hand-written tables. ts in ns.
inline OwnedTable make_trades(std::vector<int64_t> ts, std::vector<int32_t> sym, std::vector<double> price,
                              std::vector<int64_t> size) {
  OwnedTable t({{"ts", Type::I64}, {"sym", Type::Sym}, {"price", Type::F64}, {"size", Type::I64}},
               int64_t(ts.size()));
  for (size_t i = 0; i < ts.size(); ++i) {
    t.col<int64_t>("ts")[i] = ts[i];
    t.col<int32_t>("sym")[i] = sym[i];
    t.col<double>("price")[i] = price[i];
    t.col<int64_t>("size")[i] = size[i];
  }
  return t;
}

inline OwnedTable make_quotes(std::vector<int64_t> ts, std::vector<int32_t> sym, std::vector<double> bid,
                              std::vector<double> ask) {
  OwnedTable t({{"ts", Type::I64}, {"sym", Type::Sym}, {"bid", Type::F64}, {"ask", Type::F64}},
               int64_t(ts.size()));
  for (size_t i = 0; i < ts.size(); ++i) {
    t.col<int64_t>("ts")[i] = ts[i];
    t.col<int32_t>("sym")[i] = sym[i];
    t.col<double>("bid")[i] = bid[i];
    t.col<double>("ask")[i] = ask[i];
  }
  return t;
}

template <class T> std::vector<T> col_values(const Result& r, const std::string& name) {
  const Column& c = r.col(name);
  return std::vector<T>(c.as<T>(), c.as<T>() + r.rows);
}

inline Result run_query(std::string_view text, Backend b, const NamedTables& tables,
                        const std::vector<std::string>& symbols = syms()) {
  auto q = CompiledQuery::compile(std::string(text), trades_quotes_catalog(), b);
  return q->run(tables, symbols);
}

// Bitwise-equal comparison of two results (NaN == NaN).
inline ::testing::AssertionResult results_identical(const Result& a, const Result& b) {
  if (a.rows != b.rows) return ::testing::AssertionFailure() << "rows " << a.rows << " vs " << b.rows;
  if (a.cols.size() != b.cols.size()) return ::testing::AssertionFailure() << "column count differs";
  for (size_t c = 0; c < a.cols.size(); ++c) {
    if (a.cols[c].name != b.cols[c].name || a.cols[c].type != b.cols[c].type)
      return ::testing::AssertionFailure() << "column " << c << " name/type differs";
    size_t w = type_size(a.cols[c].type);
    for (int64_t i = 0; i < a.rows; ++i) {
      const std::byte* pa = a.cols[c].data.data() + size_t(i) * w;
      const std::byte* pb = b.cols[c].data.data() + size_t(i) * w;
      if (std::memcmp(pa, pb, w) != 0) {
        if (a.cols[c].type == Type::F64) {
          double x, y;
          std::memcpy(&x, pa, 8);
          std::memcpy(&y, pb, 8);
          if (std::isnan(x) && std::isnan(y)) continue;
          return ::testing::AssertionFailure() << a.cols[c].name << "[" << i << "]: " << x << " vs " << y;
        }
        return ::testing::AssertionFailure() << a.cols[c].name << "[" << i << "] differs";
      }
    }
  }
  return ::testing::AssertionSuccess();
}

} // namespace tsq::test
