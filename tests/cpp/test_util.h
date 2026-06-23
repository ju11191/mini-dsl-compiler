#pragma once

#include "binder.h"
#include "parser.h"

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
