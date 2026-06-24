// Public C++ entry point: compile a query for a backend, bind inputs, run.
#pragma once

#include "binder.h"
#include "table.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace tsq {

enum class Backend { Interp, Jit };

struct CompileStats {
  double parse_bind_ms = 0; // lexer + parser + binder
  double irgen_ms = 0;      // BoundQuery -> LLVM IR (JIT only)
  double optimize_ms = 0;   // -O2 pass pipeline (JIT only)
  double codegen_ms = 0;    // LLJIT: IR -> machine code, symbol lookup (JIT only)
  double total_ms() const { return parse_bind_ms + irgen_ms + optimize_ms + codegen_ms; }
};

struct CompileOptions {
  int opt_level = 2; // 0 = no IR optimization passes, 2 = default -O2 pipeline
};

// A table supplied by name: column name -> view.
struct NamedTable {
  int64_t rows = 0;
  std::map<std::string, ColumnView, std::less<>> cols;
};
using NamedTables = std::map<std::string, NamedTable, std::less<>>;

NamedTable named(const OwnedTable& t);

class Executable {
public:
  virtual ~Executable() = default;
  virtual Result run(const BoundInputs& in) = 0;
};

class CompiledQuery {
public:
  // Throws CompileError on syntax/type errors.
  static std::unique_ptr<CompiledQuery> compile(std::string text, const Catalog& cat, Backend backend,
                                                const CompileOptions& opts = {});

  const BoundQuery& plan() const { return plan_; }
  const std::string& text() const { return text_; }
  Backend backend() const { return backend_; }
  const CompileStats& stats() const { return stats_; }
  // LLVM IR text (JIT only; empty for the interpreter).
  const std::string& ir_unoptimized() const { return ir_unopt_; }
  const std::string& ir_optimized() const { return ir_opt_; }

  // Resolve named inputs against the plan. With validate=true, also checks that
  // `ts` is ascending where window/asof need it and that sym codes are in range.
  BoundInputs bind_inputs(const NamedTables& tables, const std::vector<std::string>& symbols,
                          bool validate = true) const;

  Result run(const BoundInputs& in) { return exec_->run(in); }
  Result run(const NamedTables& tables, const std::vector<std::string>& symbols, bool validate = true) {
    return run(bind_inputs(tables, symbols, validate));
  }

private:
  std::string text_;
  Backend backend_ = Backend::Interp;
  BoundQuery plan_;
  CompileStats stats_;
  std::string ir_unopt_, ir_opt_;
  std::unique_ptr<Executable> exec_;
};

} // namespace tsq
