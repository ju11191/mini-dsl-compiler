// Recursive-descent parser: text -> AST.
#pragma once

#include "ast.h"

#include <string_view>

namespace tsq {

// Throws CompileError with a source location on any syntax error.
Query parse(std::string_view src);

} // namespace tsq
