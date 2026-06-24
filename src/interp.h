// Reference back end: a tree-walking interpreter over the BoundQuery.
// Executes one stage at a time over materialized relations; expressions are
// evaluated per row by recursive descent over BExpr.
#pragma once

#include "binder.h"
#include "table.h"

namespace tsq {

Result interpret(const BoundQuery& q, const BoundInputs& in);

} // namespace tsq
