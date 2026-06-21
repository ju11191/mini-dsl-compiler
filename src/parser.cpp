#include "parser.h"

#include "lexer.h"

#include <sstream>

namespace tsq {

const char* binop_str(BinOp op) {
  switch (op) {
  case BinOp::Add: return "+";
  case BinOp::Sub: return "-";
  case BinOp::Mul: return "*";
  case BinOp::Div: return "/";
  case BinOp::Eq: return "==";
  case BinOp::Ne: return "!=";
  case BinOp::Lt: return "<";
  case BinOp::Le: return "<=";
  case BinOp::Gt: return ">";
  case BinOp::Ge: return ">=";
  case BinOp::And: return "and";
  case BinOp::Or: return "or";
  }
  return "?";
}

const char* aggfn_str(AggFn f) {
  switch (f) {
  case AggFn::Sum: return "sum";
  case AggFn::Mean: return "mean";
  case AggFn::Min: return "min";
  case AggFn::Max: return "max";
  case AggFn::Count: return "count";
  }
  return "?";
}

namespace {

// Span from the start of `a` to the end of `b`.
SourceLoc span(const SourceLoc& a, const SourceLoc& b) {
  SourceLoc r = a;
  r.len = (b.offset + b.len > a.offset) ? b.offset + b.len - a.offset : a.len;
  return r;
}

std::string describe(const Token& t) {
  switch (t.kind) {
  case Tok::Ident: return "identifier '" + t.text + "'";
  case Tok::Int:
  case Tok::Float:
  case Tok::Duration: return "'" + t.text + "'";
  case Tok::String: return "string \"" + t.text + "\"";
  default: return tok_name(t.kind);
  }
}

const char* kStages[] = {"filter", "select", "derive", "window", "asof_join"};
const char* kAggs[] = {"sum", "mean", "min", "max", "count"};

std::string suggest(const std::string& word, const char* const* cands, size_t n) {
  std::string best;
  size_t bestd = 3; // only suggest close matches
  for (size_t i = 0; i < n; ++i) {
    size_t d = edit_distance(word, cands[i]);
    if (d < bestd) {
      bestd = d;
      best = cands[i];
    }
  }
  return best.empty() ? "" : " (did you mean '" + best + "'?)";
}

class Parser {
public:
  explicit Parser(std::string_view src) : toks_(lex(src)) {}

  Query query() {
    Query q;
    const Token& src = expect(Tok::Ident, "a table name at the start of the query");
    q.source = src.text;
    q.source_loc = src.loc;
    while (at(Tok::Pipe)) {
      next();
      q.stages.push_back(stage());
    }
    if (!at(Tok::Eof)) {
      if (peek().kind == Tok::Ident && is_stage_name(peek().text))
        fail("expected '|' before stage '" + peek().text + "'", peek().loc);
      fail("expected '|' or end of query, found " + describe(peek()), peek().loc);
    }
    return q;
  }

private:
  std::vector<Token> toks_;
  size_t pos_ = 0;

  const Token& peek(size_t k = 0) const { return toks_[std::min(pos_ + k, toks_.size() - 1)]; }
  bool at(Tok k) const { return peek().kind == k; }
  const Token& next() {
    const Token& t = toks_[pos_];
    if (pos_ + 1 < toks_.size()) ++pos_;
    return t;
  }
  [[noreturn]] void fail(const std::string& msg, SourceLoc loc) { throw CompileError(msg, loc); }
  const Token& expect(Tok k, const std::string& what) {
    if (!at(k)) fail("expected " + what + ", found " + describe(peek()), peek().loc);
    return next();
  }
  static bool is_stage_name(const std::string& s) {
    for (auto* n : kStages)
      if (s == n) return true;
    return false;
  }

  Stage stage() {
    if (!at(Tok::Ident)) fail("expected a stage name after '|', found " + describe(peek()), peek().loc);
    const Token& kw = next();
    Stage s;
    s.loc = kw.loc;
    if (kw.text == "filter") {
      s.kind = Stage::Kind::Filter;
      if (at(Tok::Pipe) || at(Tok::Eof)) fail("'filter' needs a predicate", kw.loc);
      s.pred = expr();
    } else if (kw.text == "select" || kw.text == "derive") {
      bool is_select = kw.text == "select";
      s.kind = is_select ? Stage::Kind::Select : Stage::Kind::Derive;
      do {
        s.items.push_back(assignment(is_select));
      } while (at(Tok::Comma) && (next(), true));
    } else if (kw.text == "window") {
      s.kind = Stage::Kind::Window;
      const Token& d = expect(Tok::Duration, "a window duration such as 5m or 30s after 'window'");
      s.dur_ns = d.ival;
      s.dur_loc = d.loc;
      if (s.dur_ns <= 0) fail("window duration must be positive", d.loc);
      do {
        s.aggs.push_back(agg());
      } while (at(Tok::Comma) && (next(), true));
    } else if (kw.text == "asof_join") {
      s.kind = Stage::Kind::AsofJoin;
      const Token& t = expect(Tok::Ident, "a table name after 'asof_join'");
      s.table = t.text;
      s.table_loc = t.loc;
      if (!(at(Tok::Ident) && peek().text == "on"))
        fail("expected 'on <key>' after 'asof_join " + s.table + "', found " + describe(peek()), peek().loc);
      next();
      const Token& k = expect(Tok::Ident, "a key column after 'on'");
      s.key = k.text;
      s.key_loc = k.loc;
    } else {
      fail("unknown stage '" + kw.text + "'" + suggest(kw.text, kStages, 5) +
               "; stages are filter, select, derive, window, asof_join",
           kw.loc);
    }
    return s;
  }

  Assignment assignment(bool allow_bare) {
    const Token& name = expect(Tok::Ident, allow_bare ? "a column name or 'name = expr'" : "'name = expr'");
    Assignment a;
    a.name = name.text;
    a.name_loc = name.loc;
    if (at(Tok::Assign)) {
      next();
      a.expr = expr();
    } else if (!allow_bare) {
      fail("expected '=' after '" + name.text + "' in derive (derive name = expr)", peek().loc);
    } else if (!(at(Tok::Comma) || at(Tok::Pipe) || at(Tok::Eof))) {
      fail("computed select items need a name: write 'name = expr'", peek().loc);
    }
    return a;
  }

  Agg agg() {
    const Token& fn = expect(Tok::Ident, "an aggregate such as mean(price)");
    Agg a;
    a.loc = fn.loc;
    if (fn.text == "sum") a.fn = AggFn::Sum;
    else if (fn.text == "mean" || fn.text == "avg") a.fn = AggFn::Mean;
    else if (fn.text == "min") a.fn = AggFn::Min;
    else if (fn.text == "max") a.fn = AggFn::Max;
    else if (fn.text == "count") a.fn = AggFn::Count;
    else
      fail("unknown aggregate '" + fn.text + "'" + suggest(fn.text, kAggs, 5) +
               "; aggregates are sum, mean, min, max, count",
           fn.loc);
    expect(Tok::LParen, "'(' after '" + fn.text + "'");
    if (at(Tok::Ident)) {
      const Token& c = next();
      a.col = c.text;
      a.col_loc = c.loc;
    } else if (a.fn != AggFn::Count) {
      fail("expected a column name inside " + fn.text + "(...), found " + describe(peek()), peek().loc);
    }
    const Token& rp = expect(Tok::RParen, "')'");
    a.loc = span(a.loc, rp.loc);
    if (at(Tok::Ident) && peek().text == "as") {
      next();
      const Token& al = expect(Tok::Ident, "a column name after 'as'");
      a.alias = al.text;
      a.alias_loc = al.loc;
      a.loc = span(a.loc, al.loc);
    }
    return a;
  }

  // ---- expressions (precedence climbing by hand) ----
  ExprPtr binary(BinOp op, ExprPtr l, ExprPtr r) {
    auto e = std::make_unique<Expr>(Expr::Kind::Binary, span(l->loc, r->loc));
    e->bop = op;
    e->args.push_back(std::move(l));
    e->args.push_back(std::move(r));
    return e;
  }

  ExprPtr expr() { return or_expr(); }

  ExprPtr or_expr() {
    auto l = and_expr();
    while (at(Tok::KwOr)) {
      next();
      l = binary(BinOp::Or, std::move(l), and_expr());
    }
    return l;
  }

  ExprPtr and_expr() {
    auto l = not_expr();
    while (at(Tok::KwAnd)) {
      next();
      l = binary(BinOp::And, std::move(l), not_expr());
    }
    return l;
  }

  ExprPtr not_expr() {
    if (at(Tok::KwNot)) {
      SourceLoc l = next().loc;
      auto operand = not_expr();
      auto e = std::make_unique<Expr>(Expr::Kind::Unary, span(l, operand->loc));
      e->uop = UnOp::Not;
      e->args.push_back(std::move(operand));
      return e;
    }
    return cmp_expr();
  }

  static bool cmp_op(Tok k, BinOp& op) {
    switch (k) {
    case Tok::EqEq: op = BinOp::Eq; return true;
    case Tok::NotEq: op = BinOp::Ne; return true;
    case Tok::Lt: op = BinOp::Lt; return true;
    case Tok::Le: op = BinOp::Le; return true;
    case Tok::Gt: op = BinOp::Gt; return true;
    case Tok::Ge: op = BinOp::Ge; return true;
    default: return false;
    }
  }

  ExprPtr cmp_expr() {
    auto l = add_expr();
    BinOp op;
    if (cmp_op(peek().kind, op)) {
      next();
      l = binary(op, std::move(l), add_expr());
      if (cmp_op(peek().kind, op))
        fail("comparisons cannot be chained; use 'and' (e.g. a < b and b < c)", peek().loc);
    } else if (at(Tok::Assign)) {
      fail("'=' is assignment; use '==' to compare", peek().loc);
    }
    return l;
  }

  ExprPtr add_expr() {
    auto l = mul_expr();
    while (at(Tok::Plus) || at(Tok::Minus)) {
      BinOp op = next().kind == Tok::Plus ? BinOp::Add : BinOp::Sub;
      l = binary(op, std::move(l), mul_expr());
    }
    return l;
  }

  ExprPtr mul_expr() {
    auto l = unary_expr();
    while (at(Tok::Star) || at(Tok::Slash)) {
      BinOp op = next().kind == Tok::Star ? BinOp::Mul : BinOp::Div;
      l = binary(op, std::move(l), unary_expr());
    }
    return l;
  }

  ExprPtr unary_expr() {
    if (at(Tok::Minus)) {
      SourceLoc l = next().loc;
      auto operand = unary_expr();
      auto e = std::make_unique<Expr>(Expr::Kind::Unary, span(l, operand->loc));
      e->uop = UnOp::Neg;
      e->args.push_back(std::move(operand));
      return e;
    }
    return primary();
  }

  ExprPtr primary() {
    const Token& t = peek();
    switch (t.kind) {
    case Tok::Int: {
      next();
      auto e = std::make_unique<Expr>(Expr::Kind::IntLit, t.loc);
      e->ival = t.ival;
      return e;
    }
    case Tok::Float: {
      next();
      auto e = std::make_unique<Expr>(Expr::Kind::FloatLit, t.loc);
      e->fval = t.fval;
      return e;
    }
    case Tok::Duration: {
      next();
      auto e = std::make_unique<Expr>(Expr::Kind::DurLit, t.loc);
      e->ival = t.ival;
      return e;
    }
    case Tok::String: {
      next();
      auto e = std::make_unique<Expr>(Expr::Kind::StrLit, t.loc);
      e->name = t.text;
      return e;
    }
    case Tok::KwTrue:
    case Tok::KwFalse: {
      next();
      auto e = std::make_unique<Expr>(Expr::Kind::BoolLit, t.loc);
      e->bval = t.kind == Tok::KwTrue;
      return e;
    }
    case Tok::Ident: {
      next();
      if (at(Tok::LParen)) {
        next();
        auto e = std::make_unique<Expr>(Expr::Kind::Call, t.loc);
        e->name = t.text;
        if (!at(Tok::RParen)) {
          do {
            e->args.push_back(expr());
          } while (at(Tok::Comma) && (next(), true));
        }
        const Token& rp = expect(Tok::RParen, "')' to close the call to '" + t.text + "'");
        e->loc = span(t.loc, rp.loc);
        return e;
      }
      auto e = std::make_unique<Expr>(Expr::Kind::Column, t.loc);
      e->name = t.text;
      return e;
    }
    case Tok::LParen: {
      SourceLoc lp = next().loc;
      auto e = expr();
      const Token& rp = expect(Tok::RParen, "')'");
      e->loc = span(lp, rp.loc);
      return e;
    }
    default:
      fail("expected an expression, found " + describe(t), t.loc);
    }
  }
};

void print(std::ostream& os, const Expr& e) {
  switch (e.kind) {
  case Expr::Kind::IntLit: os << e.ival; break;
  case Expr::Kind::FloatLit: os << e.fval; break;
  case Expr::Kind::DurLit: os << e.ival << "ns"; break;
  case Expr::Kind::StrLit: os << '"' << e.name << '"'; break;
  case Expr::Kind::BoolLit: os << (e.bval ? "true" : "false"); break;
  case Expr::Kind::Column: os << e.name; break;
  case Expr::Kind::Unary:
    os << '(' << (e.uop == UnOp::Neg ? "-" : "not ");
    print(os, *e.args[0]);
    os << ')';
    break;
  case Expr::Kind::Binary:
    os << '(';
    print(os, *e.args[0]);
    os << ' ' << binop_str(e.bop) << ' ';
    print(os, *e.args[1]);
    os << ')';
    break;
  case Expr::Kind::Call:
    os << e.name << '(';
    for (size_t i = 0; i < e.args.size(); ++i) {
      if (i) os << ", ";
      print(os, *e.args[i]);
    }
    os << ')';
    break;
  }
}

} // namespace

std::string to_string(const Expr& e) {
  std::ostringstream os;
  print(os, e);
  return os.str();
}

std::string to_string(const Query& q) {
  std::ostringstream os;
  os << q.source;
  for (const auto& s : q.stages) {
    os << " | ";
    switch (s.kind) {
    case Stage::Kind::Filter: os << "filter " << to_string(*s.pred); break;
    case Stage::Kind::Select:
    case Stage::Kind::Derive:
      os << (s.kind == Stage::Kind::Select ? "select " : "derive ");
      for (size_t i = 0; i < s.items.size(); ++i) {
        if (i) os << ", ";
        os << s.items[i].name;
        if (s.items[i].expr) os << " = " << to_string(*s.items[i].expr);
      }
      break;
    case Stage::Kind::Window:
      os << "window " << s.dur_ns << "ns ";
      for (size_t i = 0; i < s.aggs.size(); ++i) {
        if (i) os << ", ";
        os << aggfn_str(s.aggs[i].fn) << '(' << s.aggs[i].col << ')';
        if (!s.aggs[i].alias.empty()) os << " as " << s.aggs[i].alias;
      }
      break;
    case Stage::Kind::AsofJoin: os << "asof_join " << s.table << " on " << s.key; break;
    }
  }
  return os.str();
}

Query parse(std::string_view src) { return Parser(src).query(); }

} // namespace tsq
