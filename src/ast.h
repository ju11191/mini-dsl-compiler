// Untyped AST produced by the parser.
#pragma once

#include "diagnostics.h"

#include <memory>
#include <string>
#include <vector>

namespace tsq {

enum class BinOp { Add, Sub, Mul, Div, Eq, Ne, Lt, Le, Gt, Ge, And, Or };
enum class UnOp { Neg, Not };

const char* binop_str(BinOp op);

struct Expr;
using ExprPtr = std::unique_ptr<Expr>;

struct Expr {
  enum class Kind { IntLit, FloatLit, StrLit, BoolLit, DurLit, Column, Unary, Binary, Call };
  Kind kind;
  SourceLoc loc; // spans the whole expression
  int64_t ival = 0;  // IntLit, DurLit (ns)
  double fval = 0;   // FloatLit
  bool bval = false; // BoolLit
  std::string name;  // Column name, StrLit contents, Call function name
  UnOp uop = UnOp::Neg;
  BinOp bop = BinOp::Add;
  std::vector<ExprPtr> args; // Unary: 1, Binary: 2, Call: n

  Expr(Kind k, SourceLoc l) : kind(k), loc(l) {}
};

enum class AggFn { Sum, Mean, Min, Max, Count };
const char* aggfn_str(AggFn f);

struct Agg {
  AggFn fn;
  SourceLoc loc;     // the whole `fn(col) [as name]`
  std::string col;   // empty for count()
  SourceLoc col_loc;
  std::string alias; // empty if none
  SourceLoc alias_loc;
};

// `name = expr` (derive/select) or a bare column `name` (select only; expr == nullptr).
struct Assignment {
  std::string name;
  SourceLoc name_loc;
  ExprPtr expr;
};

struct Stage {
  enum class Kind { Filter, Select, Derive, Window, AsofJoin };
  Kind kind;
  SourceLoc loc; // the stage keyword
  ExprPtr pred;                  // Filter
  std::vector<Assignment> items; // Select, Derive
  int64_t dur_ns = 0;            // Window
  SourceLoc dur_loc;
  std::vector<Agg> aggs;         // Window
  std::string table;             // AsofJoin
  SourceLoc table_loc;
  std::string key;
  SourceLoc key_loc;
};

struct Query {
  std::string source;
  SourceLoc source_loc;
  std::vector<Stage> stages;
};

// Canonical text form (fully parenthesized expressions); used by tests and `tsqc --ast`.
std::string to_string(const Expr& e);
std::string to_string(const Query& q);

} // namespace tsq
