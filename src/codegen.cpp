#include "codegen.h"

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/raw_ostream.h>

#include <limits>

namespace tsq {

using namespace llvm;

namespace {

// A column of the "current row" as the pipeline is being emitted. Source
// columns are loaded lazily (at first use, at the current insertion point) so
// that columns only needed after a filter are only loaded for passing rows.
struct Slot {
  Type type;
  Value* val = nullptr; // computed value, or cached load
  int src_col = -1;     // >= 0: lazily loaded source column
};

class Codegen {
public:
  Codegen(const BoundQuery& q, const CodegenOptions& opts, LLVMContext& C, Module& M)
      : q_(q), opts_(opts), C_(C), M_(M), B_(C) {}

  std::vector<ScratchSpec> emit() {
    i1_ = B_.getInt1Ty();
    i32_ = B_.getInt32Ty();
    i64_ = B_.getInt64Ty();
    f64_ = B_.getDoubleTy();
    ptr_ = B_.getPtrTy();
    ctx_ty_ = StructType::create(C_, {ptr_, ptr_, ptr_, ptr_, ptr_}, "QueryCtx");

    auto* fty = FunctionType::get(i64_, {ptr_}, false);
    F_ = Function::Create(fty, Function::ExternalLinkage, kQueryFnName, M_);
    F_->addFnAttr(Attribute::NoUnwind);
    F_->addParamAttr(0, Attribute::NoAlias);
    Value* ctx = F_->getArg(0);
    ctx->setName("ctx");

    entry_ = BasicBlock::Create(C_, "entry", F_);
    B_.SetInsertPoint(entry_);

    // Load ctx fields and every input/output pointer up front (unused ones are DCE'd).
    Value* inputs = load_field(ctx, 0, "inputs");
    Value* lens = load_field(ctx, 1, "lens");
    Value* outputs = load_field(ctx, 2, "outputs");
    scratch_base_ = load_field(ctx, 3, "scratch");
    params_ = load_field(ctx, 4, "params");
    int flat = 0;
    for (size_t t = 0; t < q_.tables.size(); ++t) {
      std::vector<Value*> cols;
      for (size_t c = 0; c < q_.tables[t].schema.size(); ++c)
        cols.push_back(load_ptr_at(inputs, flat++, q_.tables[t].name + "." + q_.tables[t].schema[c].name));
      in_ptrs_.push_back(std::move(cols));
      in_lens_.push_back(load_i64_at(lens, int(t), "n." + q_.tables[t].name));
    }
    for (size_t c = 0; c < q_.output.size(); ++c)
      out_ptrs_.push_back(load_ptr_at(outputs, int(c), "out." + q_.output[c].name));
    nsym_ = load_i64_at(params_, int(q_.sym_literals.size()), "nsym");
    for (size_t p = 0; p < q_.sym_literals.size(); ++p)
      sym_params_.push_back(B_.CreateTrunc(load_i64_at(params_, int(p), "lit." + q_.sym_literals[p]), i32_));

    o_ = alloca_init(i64_, B_.getInt64(0), "o");

    // Loop skeleton.
    BasicBlock* header = BasicBlock::Create(C_, "loop", F_);
    BasicBlock* body = BasicBlock::Create(C_, "body", F_);
    latch_ = BasicBlock::Create(C_, "latch", F_);
    BasicBlock* exit = BasicBlock::Create(C_, "exit", F_);
    Value* n = in_lens_[0];

    // Per-stage state allocas are created while emitting the body, in the entry block.
    BasicBlock* entry_end = entry_;
    B_.SetInsertPoint(header);
    PHINode* i = B_.CreatePHI(i64_, 2, "i");
    i_ = i;
    B_.CreateCondBr(B_.CreateICmpSLT(i, n), body, exit);

    B_.SetInsertPoint(body);
    std::vector<Slot> cur;
    for (size_t c = 0; c < q_.input().size(); ++c) cur.push_back(Slot{q_.input()[c].type, nullptr, int(c)});
    src_cache_.assign(q_.input().size(), nullptr);
    rows_dropped_ = false;

    for (size_t s = 0; s < q_.stages.size(); ++s) {
      const BStage& st = q_.stages[s];
      switch (st.kind) {
      case BStage::Kind::Filter: emit_filter(st, cur); break;
      case BStage::Kind::Project: emit_project(st, cur); break;
      case BStage::Kind::Window: emit_window(st, cur); break;
      case BStage::Kind::Asof: emit_asof(st, cur); break;
      }
    }

    // Store the surviving row.
    Value* o = B_.CreateLoad(i64_, o_, "o.cur");
    for (size_t c = 0; c < cur.size(); ++c) {
      Value* v = get(cur[c]);
      Value* p = B_.CreateInBoundsGEP(storage_ty(cur[c].type), out_ptrs_[c], o);
      B_.CreateStore(v, p);
    }
    B_.CreateStore(B_.CreateAdd(o, B_.getInt64(1)), o_);
    B_.CreateBr(latch_);

    B_.SetInsertPoint(latch_);
    Value* inext = B_.CreateAdd(i, B_.getInt64(1), "i.next");
    B_.CreateBr(header);
    i->addIncoming(B_.getInt64(0), entry_end);
    i->addIncoming(inext, latch_);

    B_.SetInsertPoint(entry_end);
    B_.CreateBr(header);

    B_.SetInsertPoint(exit);
    B_.CreateRet(B_.CreateLoad(i64_, o_, "o.final"));

    std::string err;
    raw_string_ostream es(err);
    if (verifyFunction(*F_, &es)) throw std::runtime_error("internal: generated invalid IR: " + es.str());
    return std::move(scratch_);
  }

private:
  const BoundQuery& q_;
  [[maybe_unused]] const CodegenOptions& opts_;
  LLVMContext& C_;
  Module& M_;
  IRBuilder<> B_;
  llvm::Type *i1_, *i32_, *i64_, *f64_, *ptr_;
  StructType* ctx_ty_;
  Function* F_;
  BasicBlock* entry_;
  BasicBlock* latch_;
  Value* i_;
  Value* o_;
  Value* scratch_base_;
  Value* params_;
  Value* nsym_;
  std::vector<Value*> sym_params_;
  std::vector<std::vector<Value*>> in_ptrs_;
  std::vector<Value*> in_lens_;
  std::vector<Value*> out_ptrs_;
  std::vector<Value*> src_cache_;
  std::vector<ScratchSpec> scratch_;
  bool rows_dropped_ = false; // has a filter run before the current point?

  // ---- helpers ----
  llvm::Type* storage_ty(Type t) {
    switch (t) {
    case Type::I64: return i64_;
    case Type::F64: return f64_;
    case Type::Sym: return i32_;
    case Type::Bool: return i1_;
    }
    return i64_;
  }
  llvm::Type* value_ty(Type t) { return storage_ty(t); }

  Value* load_field(Value* ctx, unsigned idx, const char* name) {
    return B_.CreateLoad(ptr_, B_.CreateStructGEP(ctx_ty_, ctx, idx), name);
  }
  Value* load_ptr_at(Value* base, int idx, const std::string& name) {
    return B_.CreateLoad(ptr_, B_.CreateConstInBoundsGEP1_64(ptr_, base, uint64_t(idx)), name);
  }
  Value* load_i64_at(Value* base, int idx, const std::string& name) {
    return B_.CreateLoad(i64_, B_.CreateConstInBoundsGEP1_64(i64_, base, uint64_t(idx)), name);
  }

  // Alloca at the top of the entry block, initialized at its end.
  AllocaInst* alloca_init(llvm::Type* ty, Value* init, const std::string& name) {
    IRBuilder<> ab(entry_, entry_->begin());
    AllocaInst* a = ab.CreateAlloca(ty, nullptr, name);
    // The entry block only gets its terminator once the body is done, so code
    // added to it while emitting the body is simply appended.
    IRBuilder<> sb(entry_);
    sb.CreateStore(init, a);
    return a;
  }

  // New scratch buffer; returns its pointer, loaded in the entry block.
  Value* add_scratch(ScratchSpec spec, const std::string& name) {
    int idx = int(scratch_.size());
    scratch_.push_back(spec);
    IRBuilder<> sb(entry_);
    return sb.CreateLoad(ptr_, sb.CreateConstInBoundsGEP1_64(ptr_, scratch_base_, uint64_t(idx)), name);
  }

  Value* get(Slot& s) {
    if (s.val) return s.val;
    if (!src_cache_[s.src_col]) {
      Type t = q_.input()[s.src_col].type;
      Value* p = B_.CreateInBoundsGEP(storage_ty(t), in_ptrs_[0][s.src_col], i_);
      src_cache_[s.src_col] = B_.CreateLoad(storage_ty(t), p, q_.input()[s.src_col].name);
    }
    s.val = src_cache_[s.src_col];
    return s.val;
  }

  Value* intrinsic(Intrinsic::ID id, llvm::Type* ty, std::vector<Value*> args) {
    Function* fn = Intrinsic::getOrInsertDeclaration(&M_, id, {ty});
    return B_.CreateCall(fn, args);
  }

  // ---- expressions ----
  Value* expr(const BExpr& e, std::vector<Slot>& cur) {
    switch (e.kind) {
    case BExpr::Kind::Col: return get(cur[e.col]);
    case BExpr::Kind::ConstI64: return B_.getInt64(uint64_t(e.i));
    case BExpr::Kind::ConstF64: return ConstantFP::get(f64_, e.f);
    case BExpr::Kind::ConstBool: return B_.getInt1(e.b);
    case BExpr::Kind::SymParam: return sym_params_[e.param];
    case BExpr::Kind::CastF64: return B_.CreateSIToFP(expr(*e.kids[0], cur), f64_);
    case BExpr::Kind::Neg: {
      Value* x = expr(*e.kids[0], cur);
      return e.type == Type::I64 ? B_.CreateSub(B_.getInt64(0), x) : B_.CreateFNeg(x);
    }
    case BExpr::Kind::Not: return B_.CreateXor(expr(*e.kids[0], cur), B_.getInt1(true));
    case BExpr::Kind::Abs: {
      Value* x = expr(*e.kids[0], cur);
      if (e.type == Type::I64) return intrinsic(Intrinsic::abs, i64_, {x, B_.getInt1(false)});
      return intrinsic(Intrinsic::fabs, f64_, {x});
    }
    case BExpr::Kind::Sqrt: return intrinsic(Intrinsic::sqrt, f64_, {expr(*e.kids[0], cur)});
    case BExpr::Kind::Bin: {
      Value* a = expr(*e.kids[0], cur);
      Value* b = expr(*e.kids[1], cur);
      Type t = e.kids[0]->type;
      bool fp = t == Type::F64;
      switch (e.op) {
      case BinOp::Add: return fp ? B_.CreateFAdd(a, b) : B_.CreateAdd(a, b);
      case BinOp::Sub: return fp ? B_.CreateFSub(a, b) : B_.CreateSub(a, b);
      case BinOp::Mul: return fp ? B_.CreateFMul(a, b) : B_.CreateMul(a, b);
      case BinOp::Div: return B_.CreateFDiv(a, b);
      case BinOp::Eq: return fp ? B_.CreateFCmpOEQ(a, b) : B_.CreateICmpEQ(a, b);
      case BinOp::Ne: return fp ? B_.CreateFCmpUNE(a, b) : B_.CreateICmpNE(a, b);
      case BinOp::Lt: return fp ? B_.CreateFCmpOLT(a, b) : B_.CreateICmpSLT(a, b);
      case BinOp::Le: return fp ? B_.CreateFCmpOLE(a, b) : B_.CreateICmpSLE(a, b);
      case BinOp::Gt: return fp ? B_.CreateFCmpOGT(a, b) : B_.CreateICmpSGT(a, b);
      case BinOp::Ge: return fp ? B_.CreateFCmpOGE(a, b) : B_.CreateICmpSGE(a, b);
      case BinOp::And: return B_.CreateAnd(a, b);
      case BinOp::Or: return B_.CreateOr(a, b);
      }
    }
    }
    throw std::runtime_error("internal: unhandled expression in codegen");
  }

  // ---- stages ----
  void emit_filter(const BStage& st, std::vector<Slot>& cur) {
    Value* cond = expr(*st.pred, cur);
    BasicBlock* pass = BasicBlock::Create(C_, "filter.pass", F_);
    B_.CreateCondBr(cond, pass, latch_);
    B_.SetInsertPoint(pass);
    rows_dropped_ = true;
  }

  void emit_project(const BStage& st, std::vector<Slot>& cur) {
    std::vector<Slot> next;
    for (const auto& e : st.exprs) {
      if (e->kind == BExpr::Kind::Col) next.push_back(cur[e->col]); // keeps laziness
      else next.push_back(Slot{e->type, expr(*e, cur), -1});
    }
    cur = std::move(next);
  }

  void emit_window(const BStage&, std::vector<Slot>&) {
    throw std::runtime_error("JIT: window not implemented yet");
  }

  void emit_asof(const BStage&, std::vector<Slot>&) {
    throw std::runtime_error("JIT: asof_join not implemented yet");
  }
};

} // namespace

GeneratedModule generate_ir(const BoundQuery& q, const CodegenOptions& opts, const DataLayout* dl,
                            const Triple* triple) {
  GeneratedModule gm;
  gm.ctx = std::make_unique<LLVMContext>();
  gm.module = std::make_unique<Module>("tsq", *gm.ctx);
  if (dl) gm.module->setDataLayout(*dl);
  if (triple) gm.module->setTargetTriple(*triple);
  Codegen cg(q, opts, *gm.ctx, *gm.module);
  gm.scratch = cg.emit();
  return gm;
}

} // namespace tsq
