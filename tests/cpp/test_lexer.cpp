#include "lexer.h"

#include <gtest/gtest.h>

using namespace tsq;

static std::vector<Tok> kinds(std::string_view s) {
  std::vector<Tok> k;
  for (auto& t : lex(s))
    k.push_back(t.kind);
  return k;
}

TEST(Lexer, ExampleQuery) {
  auto toks = lex(R"(trades | filter price > 100 and sym == "AAPL" | window 5m mean(price))");
  std::vector<Tok> expect = {Tok::Ident, Tok::Pipe,   Tok::Ident,    Tok::Ident, Tok::Gt,
                             Tok::Int,   Tok::KwAnd,  Tok::Ident,    Tok::EqEq,  Tok::String,
                             Tok::Pipe,  Tok::Ident,  Tok::Duration, Tok::Ident, Tok::LParen,
                             Tok::Ident, Tok::RParen, Tok::Eof};
  ASSERT_EQ(toks.size(), expect.size());
  for (size_t i = 0; i < expect.size(); ++i)
    EXPECT_EQ(toks[i].kind, expect[i]) << i;
  EXPECT_EQ(toks[9].text, "AAPL");
  EXPECT_EQ(toks[12].ival, 5LL * 60 * 1000000000);
}

TEST(Lexer, Numbers) {
  auto t = lex("42 1.5 2e3 7.25e-1");
  EXPECT_EQ(t[0].kind, Tok::Int);
  EXPECT_EQ(t[0].ival, 42);
  EXPECT_EQ(t[1].kind, Tok::Float);
  EXPECT_DOUBLE_EQ(t[1].fval, 1.5);
  EXPECT_EQ(t[2].kind, Tok::Float);
  EXPECT_DOUBLE_EQ(t[2].fval, 2000.0);
  EXPECT_DOUBLE_EQ(t[3].fval, 0.725);
}

TEST(Lexer, Durations) {
  auto t = lex("1ns 3us 100ms 30s 5m 2h 1d");
  int64_t expect[] = {
      1, 3000, 100000000, 30000000000LL, 300000000000LL, 7200000000000LL, 86400000000000LL};
  for (int i = 0; i < 7; ++i) {
    EXPECT_EQ(t[i].kind, Tok::Duration);
    EXPECT_EQ(t[i].ival, expect[i]);
  }
}

TEST(Lexer, Operators) {
  EXPECT_EQ(kinds("= == != < <= > >= + - * / , ( )"),
            (std::vector<Tok>{Tok::Assign, Tok::EqEq, Tok::NotEq, Tok::Lt, Tok::Le, Tok::Gt,
                              Tok::Ge, Tok::Plus, Tok::Minus, Tok::Star, Tok::Slash, Tok::Comma,
                              Tok::LParen, Tok::RParen, Tok::Eof}));
}

TEST(Lexer, KeywordsAndComments) {
  EXPECT_EQ(kinds("and or not true false # comment | here\nx"),
            (std::vector<Tok>{Tok::KwAnd, Tok::KwOr, Tok::KwNot, Tok::KwTrue, Tok::KwFalse,
                              Tok::Ident, Tok::Eof}));
}

TEST(Lexer, Locations) {
  auto t = lex("trades\n  | filter");
  EXPECT_EQ(t[1].loc.line, 2u);
  EXPECT_EQ(t[1].loc.col, 3u);
  EXPECT_EQ(t[2].loc.col, 5u);
  EXPECT_EQ(t[2].loc.len, 6u);
  EXPECT_EQ(t[2].loc.offset, 11u);
}

TEST(Lexer, StringEscapes) {
  auto t = lex(R"("a\"b\\c")");
  EXPECT_EQ(t[0].text, "a\"b\\c");
}

static std::string lex_error(std::string_view s) {
  try {
    lex(s);
  } catch (const CompileError& e) {
    return e.message() + " @" + std::to_string(e.loc().col);
  }
  return "no error";
}

TEST(Lexer, Errors) {
  EXPECT_EQ(lex_error("x | 5parsecs"),
            "unknown duration unit 'parsecs' (expected ns, us, ms, s, m, h, d) @5");
  EXPECT_EQ(lex_error("\"abc"), "unterminated string literal @1");
  EXPECT_EQ(lex_error("a ! b"), "unexpected '!' (use 'not' for negation, '!=' for inequality) @3");
  EXPECT_EQ(lex_error("a && b"), "unexpected '&' (use 'and') @3");
  EXPECT_EQ(lex_error("a @ b"), "unexpected character '@' @3");
  EXPECT_EQ(lex_error("1.5s"), "durations must be whole numbers, e.g. 1500ms instead of 1.5s @1");
  EXPECT_EQ(lex_error("99999999999999999999"), "integer literal is too large for i64 @1");
}
