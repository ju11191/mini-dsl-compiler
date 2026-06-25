// JIT back end: codegen -> -O2 -> ORC LLJIT -> native function, plus the
// executor that allocates outputs/scratch and calls it.
#include "codegen.h"
#include "engine.h"

#include <llvm/Analysis/CGSCCPassManager.h>
#include <llvm/Analysis/LoopAnalysisManager.h>
#include <llvm/ExecutionEngine/Orc/JITTargetMachineBuilder.h>
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/ExecutionEngine/Orc/ThreadSafeModule.h>
#include <llvm/IR/PassManager.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>

#include <algorithm>
#include <chrono>
#include <mutex>

namespace tsq {

namespace {

using Clock = std::chrono::steady_clock;
double ms_since(Clock::time_point t0) { return std::chrono::duration<double, std::milli>(Clock::now() - t0).count(); }

void init_llvm_once() {
  static std::once_flag flag;
  std::call_once(flag, [] {
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();
    llvm::InitializeNativeTargetAsmParser();
  });
}

template <class T> T check(llvm::Expected<T> v, const char* what) {
  if (!v) {
    std::string msg;
    llvm::raw_string_ostream os(msg);
    os << v.takeError();
    throw std::runtime_error(std::string(what) + ": " + os.str());
  }
  return std::move(*v);
}

void check(llvm::Error e, const char* what) {
  if (e) {
    std::string msg;
    llvm::raw_string_ostream os(msg);
    os << std::move(e);
    throw std::runtime_error(std::string(what) + ": " + os.str());
  }
}

std::string module_text(const llvm::Module& m) {
  std::string s;
  llvm::raw_string_ostream os(s);
  m.print(os, nullptr);
  return os.str();
}

void optimize(llvm::Module& m, llvm::TargetMachine* tm, int level) {
  if (level <= 0) return;
  llvm::LoopAnalysisManager lam;
  llvm::FunctionAnalysisManager fam;
  llvm::CGSCCAnalysisManager cgam;
  llvm::ModuleAnalysisManager mam;
  llvm::PassBuilder pb(tm);
  pb.registerModuleAnalyses(mam);
  pb.registerCGSCCAnalyses(cgam);
  pb.registerFunctionAnalyses(fam);
  pb.registerLoopAnalyses(lam);
  pb.crossRegisterProxies(lam, fam, cgam, mam);
  llvm::OptimizationLevel ol = level == 1 ? llvm::OptimizationLevel::O1
                               : level == 3 ? llvm::OptimizationLevel::O3
                                            : llvm::OptimizationLevel::O2;
  llvm::ModulePassManager mpm = pb.buildPerModuleDefaultPipeline(ol);
  mpm.run(m, mam);
}

class JitExecutable : public Executable {
public:
  JitExecutable(const BoundQuery& q, const CompileOptions& opts, CompileStats& stats, std::string& ir_unopt,
                std::string& ir_opt)
      : q_(q) {
    init_llvm_once();
    auto jtmb = check(llvm::orc::JITTargetMachineBuilder::detectHost(), "detect host");
    jtmb.setCodeGenOptLevel(llvm::CodeGenOptLevel::Default);
    auto tm = check(jtmb.createTargetMachine(), "create target machine");
    const llvm::DataLayout dl = tm->createDataLayout();

    auto t0 = Clock::now();
    CodegenOptions cgo;
    cgo.window_direct_input = opts.window_direct_input;
    GeneratedModule gm = generate_ir(q, cgo, &dl, &tm->getTargetTriple());
    scratch_spec_ = gm.scratch;
    stats.irgen_ms = ms_since(t0);
    if (opts.keep_ir) ir_unopt = module_text(*gm.module);

    t0 = Clock::now();
    optimize(*gm.module, tm.get(), opts.opt_level);
    stats.optimize_ms = ms_since(t0);
    if (opts.keep_ir) ir_opt = module_text(*gm.module);

    t0 = Clock::now();
    jit_ = check(llvm::orc::LLJITBuilder().setJITTargetMachineBuilder(std::move(jtmb)).create(), "create LLJIT");
    check(jit_->addIRModule(llvm::orc::ThreadSafeModule(std::move(gm.module), std::move(gm.ctx))), "add module");
    auto sym = check(jit_->lookup(kQueryFnName), "lookup");
    fn_ = sym.toPtr<QueryFn>();
    stats.codegen_ms = ms_since(t0);
  }

  int64_t run_raw(const BoundInputs& in, void* const* outputs) override {
    const int64_t n = in.tables[0].rows;
    flat_inputs_.clear();
    lens_.clear();
    for (const auto& t : in.tables) {
      for (const auto& c : t.cols) flat_inputs_.push_back(c.data);
      lens_.push_back(t.rows);
    }
    params_ = in.params;
    params_.push_back(in.nsym);
    // (Re)allocate scratch only when its size changes; refill what needs it.
    scratch_bufs_.resize(scratch_spec_.size());
    scratch_ptrs_.resize(scratch_spec_.size());
    for (size_t s = 0; s < scratch_spec_.size(); ++s) {
      const ScratchSpec& sp = scratch_spec_[s];
      int64_t len = std::max<int64_t>(1, sp.len == ScratchSpec::Len::SourceRows ? n : in.nsym);
      size_t bytes = size_t(len) * sp.elem_size;
      if (scratch_bufs_[s].bytes() != bytes) scratch_bufs_[s] = Buffer(bytes);
      if (sp.fill) std::fill_n(scratch_bufs_[s].as<int64_t>(), len, sp.fill_value);
      scratch_ptrs_[s] = scratch_bufs_[s].data();
    }
    QueryCtx ctx{flat_inputs_.data(), lens_.data(), outputs, scratch_ptrs_.data(), params_.data()};
    return fn_(&ctx);
  }

  Result run(const BoundInputs& in) override {
    const int64_t n = in.tables[0].rows;
    Result r;
    std::vector<void*> outs;
    for (const auto& c : q_.output) {
      r.cols.push_back(Column{c.name, c.type, Buffer(size_t(std::max<int64_t>(n, 1)) * type_size(c.type))});
      outs.push_back(r.cols.back().data.data());
    }
    r.rows = run_raw(in, outs.data());
    return r;
  }

private:
  const BoundQuery& q_;
  std::unique_ptr<llvm::orc::LLJIT> jit_;
  QueryFn fn_ = nullptr;
  std::vector<ScratchSpec> scratch_spec_;
  // Per-run state, reused across runs (an executable is not thread-safe).
  std::vector<const void*> flat_inputs_;
  std::vector<int64_t> lens_;
  std::vector<int64_t> params_;
  std::vector<Buffer> scratch_bufs_;
  std::vector<void*> scratch_ptrs_;
};

} // namespace

std::unique_ptr<Executable> make_jit_executable(const BoundQuery& q, const CompileOptions& opts, CompileStats& stats,
                                                std::string& ir_unopt, std::string& ir_opt) {
  return std::make_unique<JitExecutable>(q, opts, stats, ir_unopt, ir_opt);
}

} // namespace tsq
