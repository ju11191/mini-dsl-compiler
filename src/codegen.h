// BoundQuery -> LLVM IR: one function `int64_t tsq_query(QueryCtx*)` that
// runs the whole pipeline as a single fused loop over the source rows.
#pragma once

#include "binder.h"
#include "runtime.h"

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/Module.h>
#include <llvm/TargetParser/Triple.h>

#include <memory>
#include <vector>

namespace tsq {

struct CodegenOptions {
  // Read window history straight from the input arrays when no filter precedes
  // the window. Off = always copy into scratch history.
  bool window_direct_input = true;
  // When no filter follows a window, every row reaching it is written to the
  // output at the same index, so a column passed through to the output can be
  // stored there early and used as the window's history (no scratch copy).
  bool window_output_history = true;
};

struct GeneratedModule {
  std::unique_ptr<llvm::LLVMContext> ctx;
  std::unique_ptr<llvm::Module> module;
  std::vector<ScratchSpec> scratch;
};

inline constexpr const char* kQueryFnName = "tsq_query";

// `dl`/`triple`, if given, are set on the module before IR is built so loads and
// stores get the target's natural alignment.
GeneratedModule generate_ir(const BoundQuery& q, const CodegenOptions& opts = {},
                            const llvm::DataLayout* dl = nullptr, const llvm::Triple* triple = nullptr);

} // namespace tsq
