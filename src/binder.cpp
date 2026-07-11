#include "binder.h"

#include <cmath>
#include <set>
#include <sstream>

namespace tsq {

namespace {

[[noreturn]] void fail(const std::string& msg, SourceLoc loc) {
  throw CompileError(msg, loc);
}

std::string column_suggestion(const Schema& s, const std::string& name) {
  std::string best;
  size_t bestd = std::max<size_t>(2, name.size() / 3) + 1;
  for (const auto& c : s) {
    size_t d = edit_distance(name, c.name);
    if (d < bestd) {
      bestd = d;
      best = c.name;
    }
  }
  std::string out;
  if (!best.empty()) out += " (did you mean '" + best + "'?)";
  out += "; available columns: ";
  for (size_t i = 0; i < s.size(); ++i)
    out += (i ? ", " : "") + s[i].name;
  return out;
}

BExprPtr mk(BExpr::Kind k, Type t) {
  auto e = std::make_unique<BExpr>();
  e->kind = k;
  e->type = t;
  return e;
}

BExprPtr cast_f64(BExprPtr e) {
  if (e->type == Type::F64) return e;
  if (e->kind == BExpr::Kind::ConstI64) {
    auto c = mk(BExpr::Kind::ConstF64, Type::F64);
    c->f = double(e->i);
    return c;
  }
  auto c = mk(BExpr::Kind::CastF64, Type::F64);
  c->kids.push_back(std::move(e));
  return c;
}

bool is_numeric(Type t) {
  return t == Type::I64 || t == Type::F64;
}

class Binder {
public:
  Binder(const Catalog& cat) : cat_(cat) {}

  BoundQuery run(const Query& q) {
    BoundQuery bq;
    bq_ = &bq;
    bq.tables.push_back(BTable{q.source, lookup_table(q.source, q.source_loc), false});
    for (const auto& st : q.stages) {
      const Schema& in = bq.stages.empty() ? bq.tables[0].schema : bq.stages.back().out;
      bq.stages.push_back(stage(st, Schema(in)));
    }
    bq.output = bq.stages.empty() ? bq.tables[0].schema : bq.stages.back().out;
    return bq;
  }

private:
  const Catalog& cat_;
  BoundQuery* bq_ = nullptr;

  const Schema& lookup_table(const std::string& name, SourceLoc loc) {
    auto it = cat_.tables.find(name);
    if (it != cat_.tables.end()) return it->second;
    std::string msg = "unknown table '" + name + "'";
    std::string best;
    size_t bestd = 3;
    for (const auto& [n, _] : cat_.tables) {
      size_t d = edit_distance(name, n);
      if (d < bestd) {
        bestd = d;
        best = n;
      }
    }
    if (!best.empty()) msg += " (did you mean '" + best + "'?)";
    if (!cat_.tables.empty()) {
      msg += "; available tables: ";
      bool first = true;
      for (const auto& [n, _] : cat_.tables) {
        msg += (first ? "" : ", ") + n;
        first = false;
      }
    }
    fail(msg, loc);
  }

  int resolve(const Schema& s, const std::string& name, SourceLoc loc) {
    int i = find_column(s, name);
    if (i < 0) fail("unknown column '" + name + "'" + column_suggestion(s, name), loc);
    return i;
  }

  void check_storable(Type t, const std::string& name, SourceLoc loc) {
    if (t == Type::Bool)
      fail("column '" + name +
               "' would have type bool, which cannot be stored; use the condition in a filter",
           loc);
  }

  BStage stage(const Stage& st, Schema in) {
    BStage out;
    switch (st.kind) {
    case Stage::Kind::Filter: {
      out.kind = BStage::Kind::Filter;
      out.pred = expr(*st.pred, in);
      if (out.pred->type != Type::Bool)
        fail(std::string("filter predicate must be bool, but this expression is ") +
                 type_name(out.pred->type),
             st.pred->loc);
      out.out = in;
      break;
    }
    case Stage::Kind::Select:
    case Stage::Kind::Derive: {
      out.kind = BStage::Kind::Project;
      bool derive = st.kind == Stage::Kind::Derive;
      Schema result;
      if (derive) {
        for (size_t i = 0; i < in.size(); ++i) {
          auto c = mk(BExpr::Kind::Col, in[i].type);
          c->col = int(i);
          out.exprs.push_back(std::move(c));
          result.push_back(in[i]);
        }
      }
      for (const auto& item : st.items) {
        BExprPtr e;
        if (item.expr) {
          e = expr(*item.expr, in);
        } else {
          int c = resolve(in, item.name, item.name_loc);
          e = mk(BExpr::Kind::Col, in[c].type);
          e->col = c;
        }
        check_storable(e->type, item.name, item.expr ? item.expr->loc : item.name_loc);
        if (find_column(result, item.name) >= 0) {
          if (derive && find_column(in, item.name) >= 0)
            fail("column '" + item.name +
                     "' already exists; derive only adds new columns (use select to replace it)",
                 item.name_loc);
          fail("duplicate output column '" + item.name + "'", item.name_loc);
        }
        result.push_back(ColumnDef{item.name, e->type});
        out.exprs.push_back(std::move(e));
      }
      out.out = std::move(result);
      break;
    }
    case Stage::Kind::Window: {
      out.kind = BStage::Kind::Window;
      out.dur_ns = st.dur_ns;
      out.ts_col = ts_column(in, st.loc, "window");
      bq_->tables[0].needs_sorted_ts = true;
      out.out = in;
      for (const auto& a : st.aggs) {
        BAgg b;
        b.fn = a.fn;
        if (a.fn == AggFn::Count) {
          b.col = a.col.empty() ? -1 : resolve(in, a.col, a.col_loc);
          b.in_type = Type::I64;
          b.out_type = Type::I64;
          b.name = a.alias.empty() ? "count" : a.alias;
          b.col = -1; // count() ignores its argument: every row in the window counts
        } else {
          b.col = resolve(in, a.col, a.col_loc);
          b.in_type = in[b.col].type;
          if (!is_numeric(b.in_type))
            fail(std::string(aggfn_str(a.fn)) + "() needs a numeric column, but '" + a.col +
                     "' is " + type_name(b.in_type),
                 a.col_loc);
          b.out_type = a.fn == AggFn::Mean ? Type::F64 : b.in_type;
          b.name = a.alias.empty() ? std::string(aggfn_str(a.fn)) + "_" + a.col : a.alias;
        }
        if (find_column(out.out, b.name) >= 0)
          fail("window output column '" + b.name + "' already exists; rename it with 'as'", a.loc);
        out.out.push_back(ColumnDef{b.name, b.out_type});
        out.aggs.push_back(std::move(b));
      }
      break;
    }
    case Stage::Kind::AsofJoin: {
      out.kind = BStage::Kind::Asof;
      out.ts_col = ts_column(in, st.loc, "asof_join");
      bq_->tables[0].needs_sorted_ts = true;
      const Schema& right = lookup_table(st.table, st.table_loc);
      // Deduplicate right tables (the same table may be joined twice).
      int rt = -1;
      for (size_t i = 1; i < bq_->tables.size(); ++i)
        if (bq_->tables[i].name == st.table) rt = int(i);
      if (rt < 0) {
        bq_->tables.push_back(BTable{st.table, right, true});
        rt = int(bq_->tables.size() - 1);
      }
      out.right_table = rt;
      out.left_key = find_column(in, st.key);
      if (out.left_key < 0)
        fail("join key '" + st.key + "' is not a column of the left side" +
                 column_suggestion(in, st.key),
             st.key_loc);
      out.right_key = find_column(right, st.key);
      if (out.right_key < 0)
        fail("join key '" + st.key + "' is not a column of table '" + st.table + "'" +
                 column_suggestion(right, st.key),
             st.key_loc);
      if (in[out.left_key].type != Type::Sym || right[out.right_key].type != Type::Sym)
        fail("asof_join key must be a sym column on both sides (left is " +
                 std::string(type_name(in[out.left_key].type)) + ", right is " +
                 type_name(right[out.right_key].type) + ")",
             st.key_loc);
      out.right_ts = find_column(right, "ts");
      if (out.right_ts < 0 || right[out.right_ts].type != Type::I64)
        fail("table '" + st.table + "' needs an i64 'ts' column for asof_join", st.table_loc);
      out.out = in;
      for (size_t c = 0; c < right.size(); ++c) {
        if (int(c) == out.right_ts || int(c) == out.right_key) continue;
        if (right[c].type == Type::Sym)
          fail("asof_join cannot carry sym column '" + right[c].name + "' from table '" + st.table +
                   "' (only i64/f64 payload columns are supported)",
               st.table_loc);
        std::string name = right[c].name;
        if (find_column(out.out, name) >= 0) name += "_" + st.table;
        if (find_column(out.out, name) >= 0)
          fail("asof_join output column '" + name + "' already exists", st.table_loc);
        out.payload.push_back(BPayload{int(c), right[c].type, name});
        out.out.push_back(ColumnDef{name, Type::F64});
      }
      break;
    }
    }
    return out;
  }

  int ts_column(const Schema& in, SourceLoc loc, const char* what) {
    int ts = find_column(in, "ts");
    if (ts < 0)
      fail(std::string(what) + " needs an i64 'ts' column, but the input to this stage has none" +
               " (was it dropped by an earlier select?)",
           loc);
    if (in[ts].type != Type::I64)
      fail(std::string(what) + " needs 'ts' to be i64 nanoseconds, but it is " +
               type_name(in[ts].type),
           loc);
    return ts;
  }

  int sym_param(const std::string& s) {
    auto& lits = bq_->sym_literals;
    for (size_t i = 0; i < lits.size(); ++i)
      if (lits[i] == s) return int(i);
    lits.push_back(s);
    return int(lits.size() - 1);
  }

  BExprPtr expr(const Expr& e, const Schema& in) {
    switch (e.kind) {
    case Expr::Kind::IntLit:
    case Expr::Kind::DurLit: {
      auto b = mk(BExpr::Kind::ConstI64, Type::I64);
      b->i = e.ival;
      return b;
    }
    case Expr::Kind::FloatLit: {
      auto b = mk(BExpr::Kind::ConstF64, Type::F64);
      b->f = e.fval;
      return b;
    }
    case Expr::Kind::BoolLit: {
      auto b = mk(BExpr::Kind::ConstBool, Type::Bool);
      b->b = e.bval;
      return b;
    }
    case Expr::Kind::StrLit:
      fail("string literals can only be compared with sym columns (e.g. sym == \"AAPL\")", e.loc);
    case Expr::Kind::Column: {
      int c = resolve(in, e.name, e.loc);
      auto b = mk(BExpr::Kind::Col, in[c].type);
      b->col = c;
      return b;
    }
    case Expr::Kind::Unary: {
      auto x = expr(*e.args[0], in);
      if (e.uop == UnOp::Neg) {
        if (!is_numeric(x->type))
          fail(std::string("unary '-' needs a numeric operand, found ") + type_name(x->type),
               e.loc);
        auto b = mk(BExpr::Kind::Neg, x->type);
        b->kids.push_back(std::move(x));
        return b;
      }
      if (x->type != Type::Bool)
        fail(std::string("'not' needs a bool operand, found ") + type_name(x->type), e.loc);
      auto b = mk(BExpr::Kind::Not, Type::Bool);
      b->kids.push_back(std::move(x));
      return b;
    }
    case Expr::Kind::Call: {
      bool is_abs = e.name == "abs", is_sqrt = e.name == "sqrt";
      if (!is_abs && !is_sqrt) {
        std::string hint;
        if (e.name == "sum" || e.name == "mean" || e.name == "min" || e.name == "max" ||
            e.name == "count")
          hint = " ('" + e.name + "' is an aggregate; use it in a window stage)";
        fail("unknown function '" + e.name + "'" + hint + "; functions are abs, sqrt", e.loc);
      }
      if (e.args.size() != 1) fail(e.name + "() takes exactly 1 argument", e.loc);
      auto x = expr(*e.args[0], in);
      if (!is_numeric(x->type))
        fail(e.name + "() needs a numeric argument, found " + type_name(x->type), e.args[0]->loc);
      if (is_sqrt) x = cast_f64(std::move(x));
      auto b = mk(is_abs ? BExpr::Kind::Abs : BExpr::Kind::Sqrt, x->type);
      b->kids.push_back(std::move(x));
      return b;
    }
    case Expr::Kind::Binary:
      return binary(e, in);
    }
    fail("internal: unhandled expression", e.loc);
  }

  BExprPtr binary(const Expr& e, const Schema& in) {
    const Expr& le = *e.args[0];
    const Expr& re = *e.args[1];
    BinOp op = e.bop;
    bool is_cmp = op == BinOp::Eq || op == BinOp::Ne || op == BinOp::Lt || op == BinOp::Le ||
                  op == BinOp::Gt || op == BinOp::Ge;
    bool is_eq = op == BinOp::Eq || op == BinOp::Ne;

    // sym == "literal" / "literal" == sym: bind the literal as a run-time parameter.
    if (is_cmp && (le.kind == Expr::Kind::StrLit || re.kind == Expr::Kind::StrLit)) {
      const Expr& lit = le.kind == Expr::Kind::StrLit ? le : re;
      const Expr& other = le.kind == Expr::Kind::StrLit ? re : le;
      if (other.kind == Expr::Kind::StrLit) fail("cannot compare two string literals", e.loc);
      auto o = expr(other, in);
      if (o->type != Type::Sym)
        fail(std::string("cannot compare ") + type_name(o->type) + " with a string literal", e.loc);
      if (!is_eq)
        fail(std::string("sym values only support == and !=, not '") + binop_str(op) + "'", e.loc);
      auto p = mk(BExpr::Kind::SymParam, Type::Sym);
      p->param = sym_param(lit.name);
      auto b = mk(BExpr::Kind::Bin, Type::Bool);
      b->op = op;
      b->kids.push_back(std::move(o));
      b->kids.push_back(std::move(p));
      return b;
    }

    auto l = expr(le, in);
    auto r = expr(re, in);
    auto mismatch = [&](const char* what) {
      fail(std::string("operator '") + binop_str(op) + "' " + what + ", found " +
               type_name(l->type) + " and " + type_name(r->type),
           e.loc);
    };

    if (op == BinOp::And || op == BinOp::Or) {
      if (l->type != Type::Bool || r->type != Type::Bool) mismatch("needs bool operands");
      auto b = mk(BExpr::Kind::Bin, Type::Bool);
      b->op = op;
      b->kids.push_back(std::move(l));
      b->kids.push_back(std::move(r));
      return b;
    }

    Type result;
    if (is_cmp) {
      if (is_numeric(l->type) && is_numeric(r->type)) {
        if (l->type != r->type) {
          l = cast_f64(std::move(l));
          r = cast_f64(std::move(r));
        }
      } else if (l->type == r->type && (l->type == Type::Sym || l->type == Type::Bool)) {
        if (!is_eq)
          fail(std::string(type_name(l->type)) + " values only support == and !=, not '" +
                   binop_str(op) + "'",
               e.loc);
      } else {
        mismatch("cannot compare these types");
      }
      result = Type::Bool;
    } else {
      if (!is_numeric(l->type) || !is_numeric(r->type)) mismatch("needs numeric operands");
      if (op == BinOp::Div || l->type != r->type) {
        // '/' is always floating point (no integer division by zero).
        l = cast_f64(std::move(l));
        r = cast_f64(std::move(r));
      }
      result = l->type;
    }
    auto b = mk(BExpr::Kind::Bin, result);
    b->op = op;
    b->kids.push_back(std::move(l));
    b->kids.push_back(std::move(r));
    return b;
  }
};

void print(std::ostream& os, const BExpr& e, const Schema& in, const BoundQuery& q) {
  switch (e.kind) {
  case BExpr::Kind::Col:
    os << in[e.col].name;
    break;
  case BExpr::Kind::ConstI64:
    os << e.i;
    break;
  case BExpr::Kind::ConstF64:
    os << e.f << "f";
    break;
  case BExpr::Kind::ConstBool:
    os << (e.b ? "true" : "false");
    break;
  case BExpr::Kind::SymParam:
    os << "sym\"" << q.sym_literals[e.param] << "\"";
    break;
  case BExpr::Kind::CastF64:
    os << "f64(";
    print(os, *e.kids[0], in, q);
    os << ")";
    break;
  case BExpr::Kind::Neg:
  case BExpr::Kind::Not:
  case BExpr::Kind::Abs:
  case BExpr::Kind::Sqrt: {
    const char* n = e.kind == BExpr::Kind::Neg   ? "-"
                    : e.kind == BExpr::Kind::Not ? "not "
                    : e.kind == BExpr::Kind::Abs ? "abs"
                                                 : "sqrt";
    os << n << "(";
    print(os, *e.kids[0], in, q);
    os << ")";
    break;
  }
  case BExpr::Kind::Bin:
    os << "(";
    print(os, *e.kids[0], in, q);
    os << " " << binop_str(e.op) << " ";
    print(os, *e.kids[1], in, q);
    os << "):" << type_name(e.type);
    break;
  }
}

} // namespace

std::string to_string(const BExpr& e, const Schema& in, const BoundQuery& q) {
  std::ostringstream os;
  print(os, e, in, q);
  return os.str();
}

BoundQuery bind(const Query& q, const Catalog& cat) {
  return Binder(cat).run(q);
}

} // namespace tsq
