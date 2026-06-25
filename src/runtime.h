// ABI between generated code and the C++ executor.
#pragma once

#include <cstddef>
#include <cstdint>

namespace tsq {

// Passed to the generated function `int64_t tsq_query(QueryCtx*)`.
// Field order is mirrored by the LLVM struct type in codegen.cpp.
struct QueryCtx {
  const void* const* inputs;  // every column of every BoundQuery table, flattened in order
  const int64_t* input_lens;  // rows per BoundQuery table
  void* const* outputs;       // one buffer per output column (capacity = source rows)
  void* const* scratch;       // buffers described by ScratchSpec
  const int64_t* params;      // sym literal codes, then nsym
};

using QueryFn = int64_t (*)(QueryCtx*);

// A scratch buffer the generated code needs (window history, deques, asof last[]).
struct ScratchSpec {
  enum class Len { SourceRows, NSym };
  size_t elem_size = 8;
  Len len = Len::SourceRows;
  bool fill = false; // initialize every element to fill_value (int64) before each run
  int64_t fill_value = 0;
};

} // namespace tsq
