// Hand-written lexer for the tsq query language.
#pragma once

#include "diagnostics.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace tsq {

enum class Tok {
  Eof,
  Ident,    // trades, price, filter, mean ...  (stage names are contextual)
  Int,      // 100
  Float,    // 1.5, 2e3
  String,   // "AAPL"
  Duration, // 5m, 100ms  (value normalized to nanoseconds)
  Pipe,     // |
  Comma,
  LParen,
  RParen,
  Assign, // =
  EqEq,   // ==
  NotEq,  // !=
  Lt,
  Le,
  Gt,
  Ge,
  Plus,
  Minus,
  Star,
  Slash,
  // reserved words
  KwAnd,
  KwOr,
  KwNot,
  KwTrue,
  KwFalse,
};

const char* tok_name(Tok t);

struct Token {
  Tok kind = Tok::Eof;
  SourceLoc loc;
  std::string text; // identifier name / string contents (unescaped) / raw number
  int64_t ival = 0; // Int value, or Duration in ns
  double fval = 0;  // Float value
};

// Tokenizes the whole input up front. Throws CompileError on bad input.
std::vector<Token> lex(std::string_view src);

} // namespace tsq
