// Source locations and compile errors with caret-style rendering.
#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

namespace tsq {

struct SourceLoc {
  uint32_t offset = 0; // byte offset into the query text
  uint32_t line = 1;   // 1-based
  uint32_t col = 1;    // 1-based
  uint32_t len = 1;    // length of the highlighted span (>= 1)
};

// Thrown by the lexer, parser and binder. what() is the plain message;
// render() produces a multi-line message with the offending source underlined.
class CompileError : public std::runtime_error {
public:
  CompileError(std::string msg, SourceLoc loc) : std::runtime_error(msg), msg_(std::move(msg)), loc_(loc) {}
  const SourceLoc& loc() const { return loc_; }
  const std::string& message() const { return msg_; }
  std::string render(std::string_view source) const;

private:
  std::string msg_;
  SourceLoc loc_;
};

// Thrown for problems with the data passed to run() (lengths, unsorted ts...).
class RuntimeError : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

// Levenshtein distance, used for "did you mean" suggestions.
size_t edit_distance(std::string_view a, std::string_view b);

} // namespace tsq
