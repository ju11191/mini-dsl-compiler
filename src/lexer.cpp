#include "lexer.h"

#include <cctype>
#include <charconv>
#include <cstdlib>
#include <limits>

namespace tsq {

const char* tok_name(Tok t) {
  switch (t) {
  case Tok::Eof: return "end of query";
  case Tok::Ident: return "identifier";
  case Tok::Int: return "integer";
  case Tok::Float: return "number";
  case Tok::String: return "string";
  case Tok::Duration: return "duration";
  case Tok::Pipe: return "'|'";
  case Tok::Comma: return "','";
  case Tok::LParen: return "'('";
  case Tok::RParen: return "')'";
  case Tok::Assign: return "'='";
  case Tok::EqEq: return "'=='";
  case Tok::NotEq: return "'!='";
  case Tok::Lt: return "'<'";
  case Tok::Le: return "'<='";
  case Tok::Gt: return "'>'";
  case Tok::Ge: return "'>='";
  case Tok::Plus: return "'+'";
  case Tok::Minus: return "'-'";
  case Tok::Star: return "'*'";
  case Tok::Slash: return "'/'";
  case Tok::KwAnd: return "'and'";
  case Tok::KwOr: return "'or'";
  case Tok::KwNot: return "'not'";
  case Tok::KwTrue: return "'true'";
  case Tok::KwFalse: return "'false'";
  }
  return "?";
}

namespace {

struct Lexer {
  std::string_view src;
  size_t pos = 0;
  uint32_t line = 1, col = 1;

  bool eof() const { return pos >= src.size(); }
  char peek(size_t k = 0) const { return pos + k < src.size() ? src[pos + k] : '\0'; }
  void advance() {
    if (src[pos] == '\n') {
      ++line;
      col = 1;
    } else {
      ++col;
    }
    ++pos;
  }
  SourceLoc here() const { return SourceLoc{uint32_t(pos), line, col, 1}; }

  [[noreturn]] void fail(const std::string& msg, SourceLoc loc) { throw CompileError(msg, loc); }

  static bool ident_start(char c) { return std::isalpha((unsigned char)c) || c == '_'; }
  static bool ident_char(char c) { return std::isalnum((unsigned char)c) || c == '_'; }

  // Returns the number of nanoseconds per unit, or 0 if `u` is not a unit.
  static int64_t unit_ns(std::string_view u) {
    if (u == "ns") return 1;
    if (u == "us") return 1000;
    if (u == "ms") return 1000000;
    if (u == "s") return 1000000000LL;
    if (u == "m") return 60LL * 1000000000LL;
    if (u == "h") return 3600LL * 1000000000LL;
    if (u == "d") return 86400LL * 1000000000LL;
    return 0;
  }

  Token number(SourceLoc start) {
    size_t b = pos;
    bool is_float = false;
    while (std::isdigit((unsigned char)peek())) advance();
    if (peek() == '.' && std::isdigit((unsigned char)peek(1))) {
      is_float = true;
      advance();
      while (std::isdigit((unsigned char)peek())) advance();
    }
    if ((peek() == 'e' || peek() == 'E') &&
        (std::isdigit((unsigned char)peek(1)) ||
         ((peek(1) == '+' || peek(1) == '-') && std::isdigit((unsigned char)peek(2))))) {
      is_float = true;
      advance();
      if (peek() == '+' || peek() == '-') advance();
      while (std::isdigit((unsigned char)peek())) advance();
    }
    std::string text(src.substr(b, pos - b));
    Token t;
    t.text = text;
    // A unit suffix directly after an integer makes a duration: 5m, 100ms.
    if (ident_start(peek())) {
      size_t ub = pos;
      while (ident_char(peek())) advance();
      std::string_view unit = src.substr(ub, pos - ub);
      start.len = uint32_t(pos - start.offset);
      int64_t mult = unit_ns(unit);
      if (mult == 0)
        fail("unknown duration unit '" + std::string(unit) + "' (expected ns, us, ms, s, m, h, d)", start);
      if (is_float) fail("durations must be whole numbers, e.g. 1500ms instead of 1.5s", start);
      int64_t v = 0;
      auto r = std::from_chars(text.data(), text.data() + text.size(), v);
      if (r.ec != std::errc() || v > std::numeric_limits<int64_t>::max() / mult)
        fail("duration is too large", start);
      t.kind = Tok::Duration;
      t.ival = v * mult;
      t.text = std::string(src.substr(start.offset, start.len));
      t.loc = start;
      return t;
    }
    start.len = uint32_t(pos - start.offset);
    t.loc = start;
    if (is_float) {
      t.kind = Tok::Float;
      t.fval = std::strtod(text.c_str(), nullptr);
    } else {
      t.kind = Tok::Int;
      auto r = std::from_chars(text.data(), text.data() + text.size(), t.ival);
      if (r.ec != std::errc()) fail("integer literal is too large for i64", start);
    }
    return t;
  }

  Token string(SourceLoc start) {
    advance(); // opening quote
    std::string out;
    while (true) {
      if (eof() || peek() == '\n') {
        start.len = uint32_t(pos - start.offset);
        fail("unterminated string literal", start);
      }
      char c = peek();
      if (c == '"') {
        advance();
        break;
      }
      if (c == '\\') {
        SourceLoc esc = here();
        advance();
        char e = peek();
        if (e == '"' || e == '\\') {
          out += e;
          advance();
        } else if (e == 'n') {
          out += '\n';
          advance();
        } else {
          esc.len = 2;
          fail("unknown escape sequence in string", esc);
        }
        continue;
      }
      out += c;
      advance();
    }
    start.len = uint32_t(pos - start.offset);
    Token t;
    t.kind = Tok::String;
    t.loc = start;
    t.text = std::move(out);
    return t;
  }

  Token next() {
    // Skip whitespace and '#' comments.
    while (!eof()) {
      char c = peek();
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
        advance();
      } else if (c == '#') {
        while (!eof() && peek() != '\n') advance();
      } else {
        break;
      }
    }
    SourceLoc start = here();
    Token t;
    t.loc = start;
    if (eof()) {
      t.kind = Tok::Eof;
      return t;
    }
    char c = peek();
    if (std::isdigit((unsigned char)c)) return number(start);
    if (c == '"') return string(start);
    if (ident_start(c)) {
      size_t b = pos;
      while (ident_char(peek())) advance();
      t.text = std::string(src.substr(b, pos - b));
      t.loc.len = uint32_t(pos - b);
      if (t.text == "and") t.kind = Tok::KwAnd;
      else if (t.text == "or") t.kind = Tok::KwOr;
      else if (t.text == "not") t.kind = Tok::KwNot;
      else if (t.text == "true") t.kind = Tok::KwTrue;
      else if (t.text == "false") t.kind = Tok::KwFalse;
      else t.kind = Tok::Ident;
      return t;
    }
    auto one = [&](Tok k) {
      advance();
      t.kind = k;
      return t;
    };
    auto two = [&](Tok k) {
      advance();
      advance();
      t.kind = k;
      t.loc.len = 2;
      return t;
    };
    switch (c) {
    case '|': return one(Tok::Pipe);
    case ',': return one(Tok::Comma);
    case '(': return one(Tok::LParen);
    case ')': return one(Tok::RParen);
    case '+': return one(Tok::Plus);
    case '-': return one(Tok::Minus);
    case '*': return one(Tok::Star);
    case '/': return one(Tok::Slash);
    case '=': return peek(1) == '=' ? two(Tok::EqEq) : one(Tok::Assign);
    case '!':
      if (peek(1) == '=') return two(Tok::NotEq);
      fail("unexpected '!' (use 'not' for negation, '!=' for inequality)", start);
    case '<': return peek(1) == '=' ? two(Tok::Le) : one(Tok::Lt);
    case '>': return peek(1) == '=' ? two(Tok::Ge) : one(Tok::Gt);
    case '&':
      fail("unexpected '&' (use 'and')", start);
    default: break;
    }
    std::string shown = std::isprint((unsigned char)c) ? std::string(1, c) : "\\x" + std::to_string(int((unsigned char)c));
    fail("unexpected character '" + shown + "'", start);
  }
};

} // namespace

std::vector<Token> lex(std::string_view src) {
  Lexer lx{src};
  std::vector<Token> out;
  while (true) {
    out.push_back(lx.next());
    if (out.back().kind == Tok::Eof) break;
  }
  return out;
}

} // namespace tsq
