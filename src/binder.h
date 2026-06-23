// Binder / type checker: AST + Catalog -> BoundQuery, the typed plan that
// both back ends (interpreter and JIT) execute.
#pragma once

#include "ast.h"
#include "types.h"

#include <memory>
#include <string_view>

namespace tsq {

struct BExpr;
using BExprPtr = std::unique_ptr<BExpr>;

struct BExpr {
  enum class Kind {
    Col,      // column `col` of the stage's input schema
    ConstI64, // i
    ConstF64, // f
    ConstBool,// b
    SymParam, // code of sym literal #param, supplied at run time via QueryCtx::params
    CastF64,  // i64 -> f64
    Neg,
    Not,
    Bin,      // op, kids[0], kids[1]; operands already have identical types
    Abs,
    Sqrt,
  };
  Kind kind;
  Type type;
  int col = -1;
  int64_t i = 0;
  double f = 0;
  bool b = false;
  int param = -1;
  BinOp op = BinOp::Add;
  std::vector<BExprPtr> kids;
};

struct BAgg {
  AggFn fn;
  int col = -1; // input column (-1 for count)
  Type in_type = Type::I64;
  Type out_type = Type::I64;
  std::string name;
};

struct BPayload {
  int right_col;  // column index in the right table
  Type in_type;   // i64 or f64; output is always f64 (NaN when unmatched)
  std::string name;
};

struct BStage {
  enum class Kind { Filter, Project, Window, Asof };
  Kind kind;
  BExprPtr pred;               // Filter
  std::vector<BExprPtr> exprs; // Project: output column i = exprs[i]
  int64_t dur_ns = 0;          // Window
  int ts_col = -1;             // Window, Asof: left ts column
  std::vector<BAgg> aggs;      // Window: appended after the input columns
  int right_table = -1;        // Asof: index into BoundQuery::tables
  int left_key = -1, right_key = -1, right_ts = -1;
  std::vector<BPayload> payload; // Asof: appended after the input columns
  Schema out;                  // schema after this stage
};

struct BTable {
  std::string name;
  Schema schema;
  bool needs_sorted_ts = false; // window/asof require ts ascending
};

struct BoundQuery {
  std::vector<BTable> tables; // [0] = source table, then asof right tables (deduplicated)
  std::vector<BStage> stages;
  Schema output;
  std::vector<std::string> sym_literals; // SymParam i resolves to the code of sym_literals[i]
  const Schema& input() const { return tables[0].schema; }
  // Schema seen by stage i (the input of stage i).
  const Schema& stage_input(size_t i) const { return i == 0 ? input() : stages[i - 1].out; }
};

// Throws CompileError (with source location) on any semantic error.
BoundQuery bind(const Query& q, const Catalog& cat);

std::string to_string(const BExpr& e, const Schema& in, const BoundQuery& q);

} // namespace tsq
