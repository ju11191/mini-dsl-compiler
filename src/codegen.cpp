#include "codegen.h"

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/raw_ostream.h>

#include <limits>
#include <map>

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

    prestored_.assign(q_.output.size(), false);
    for (size_t s = 0; s < q_.stages.size(); ++s) {
      const BStage& st = q_.stages[s];
      stage_idx_ = s;
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
      if (prestored_[c]) continue; // already written by a window stage
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
  const CodegenOptions& opts_;
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
  size_t stage_idx_ = 0;
  std::vector<bool> prestored_; // output columns already stored for this row

  // Index in the final output of column `c` of stage `s`'s output, if it is
  // passed through unchanged by every later stage; -1 otherwise.
  int trace_to_output(size_t s, int c) const {
    for (size_t t = s + 1; t < q_.stages.size() && c >= 0; ++t) {
      const BStage& st = q_.stages[t];
      if (st.kind != BStage::Kind::Project) continue; // filter/window/asof keep indices
      int next = -1;
      for (size_t j = 0; j < st.exprs.size(); ++j)
        if (st.exprs[j]->kind == BExpr::Kind::Col && st.exprs[j]->col == c) {
          next = int(j);
          break;
        }
      c = next;
    }
    return c;
  }
  bool filter_after(size_t s) const {
    for (size_t t = s + 1; t < q_.stages.size(); ++t)
      if (q_.stages[t].kind == BStage::Kind::Filter) return true;
    return false;
  }

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

  BasicBlock* block(const char* name) { return BasicBlock::Create(C_, name, F_); }
  Value* gep_load(llvm::Type* ty, Value* base, Value* idx, const char* name = "") {
    return B_.CreateLoad(ty, B_.CreateInBoundsGEP(ty, base, idx), name);
  }
  void gep_store(Value* v, Value* base, Value* idx) {
    B_.CreateStore(v, B_.CreateInBoundsGEP(v->getType(), base, idx));
  }

  // Trailing time window (ts - dur, ts], updated incrementally per row that
  // reaches this stage. Mirrors Interp::window operation-for-operation so FP
  // results are bit-identical.
  void emit_window(const BStage& st, std::vector<Slot>& cur) {
    // Materialize every current-row value we need before emitting inner loops
    // (lazy loads must be emitted in a block that dominates later uses).
    Value* t = get(cur[st.ts_col]);
    std::map<int, Value*> xval;  // agg input column -> current value
    std::map<int, Value*> xhist; // agg input column -> history buffer
    for (const auto& a : st.aggs)
      if (a.col >= 0 && !xval.count(a.col)) xval[a.col] = get(cur[a.col]);

    // History buffers. If no filter ran before this stage, row k of the
    // window's input is source row k, so a column passed straight through from
    // the source can be read from the input array instead of being copied.
    auto direct = [&](int c) -> Value* {
      if (!opts_.window_direct_input || rows_dropped_ || cur[c].src_col < 0) return nullptr;
      return in_ptrs_[0][cur[c].src_col];
    };
    AllocaInst* kA = alloca_init(i64_, B_.getInt64(0), "win.k");
    AllocaInst* startA = alloca_init(i64_, B_.getInt64(0), "win.start");
    Value* k = B_.CreateLoad(i64_, kA, "k");
    // Otherwise, if no filter follows, row k of this stage is output row k:
    // store the column to the output now and read history back from there.
    const bool out_ok = opts_.window_output_history && !filter_after(stage_idx_);
    auto out_hist = [&](int c, Value* v) -> Value* {
      int oc = out_ok ? trace_to_output(stage_idx_, c) : -1;
      if (oc < 0) return nullptr;
      gep_store(v, out_ptrs_[oc], k);
      prestored_[oc] = true;
      return out_ptrs_[oc];
    };
    Value* hts = direct(st.ts_col);
    if (!hts) hts = out_hist(st.ts_col, t);
    if (!hts) {
      hts = add_scratch({8, ScratchSpec::Len::SourceRows, false, 0}, "win.hist.ts");
      gep_store(t, hts, k);
    }
    for (const auto& [c, v] : xval) {
      xhist[c] = direct(c);
      if (!xhist[c]) xhist[c] = out_hist(c, v);
      if (!xhist[c]) {
        xhist[c] = add_scratch({8, ScratchSpec::Len::SourceRows, false, 0}, "win.hist." + cur_name(st, c));
        gep_store(v, xhist[c], k);
      }
    }
    Value* lo = intrinsic(Intrinsic::ssub_sat, i64_, {t, B_.getInt64(uint64_t(st.dur_ns))});

    // Per-aggregate state.
    size_t na = st.aggs.size();
    std::vector<AllocaInst*> sumA(na, nullptr), headA(na, nullptr), tailA(na, nullptr);
    std::vector<Value*> dq(na, nullptr);
    for (size_t a = 0; a < na; ++a) {
      const BAgg& g = st.aggs[a];
      llvm::Type* ty = value_ty(g.in_type);
      if (g.fn == AggFn::Sum || g.fn == AggFn::Mean)
        sumA[a] = alloca_init(ty, Constant::getNullValue(ty), "win.sum");
      if (g.fn == AggFn::Min || g.fn == AggFn::Max) {
        headA[a] = alloca_init(i64_, B_.getInt64(0), "win.dq.head");
        tailA[a] = alloca_init(i64_, B_.getInt64(0), "win.dq.tail");
        dq[a] = add_scratch({8, ScratchSpec::Len::SourceRows, false, 0}, "win.dq");
      }
    }

    // 1. Evict rows with ts <= lo.
    BasicBlock* ev_cond = block("win.evict.cond");
    BasicBlock* ev_body = block("win.evict.body");
    BasicBlock* ev_done = block("win.evict.done");
    B_.CreateBr(ev_cond);
    B_.SetInsertPoint(ev_cond);
    Value* s = B_.CreateLoad(i64_, startA, "start");
    Value* ts_s = gep_load(i64_, hts, s, "ts.old");
    B_.CreateCondBr(B_.CreateICmpSLE(ts_s, lo), ev_body, ev_done);
    B_.SetInsertPoint(ev_body);
    for (size_t a = 0; a < na; ++a) {
      if (!sumA[a]) continue;
      const BAgg& g = st.aggs[a];
      llvm::Type* ty = value_ty(g.in_type);
      Value* x_old = gep_load(ty, xhist[g.col], s, "x.old");
      Value* sum = B_.CreateLoad(ty, sumA[a]);
      B_.CreateStore(g.in_type == Type::F64 ? B_.CreateFSub(sum, x_old) : B_.CreateSub(sum, x_old), sumA[a]);
    }
    B_.CreateStore(B_.CreateAdd(s, B_.getInt64(1)), startA);
    B_.CreateBr(ev_cond);

    // 2. Add row k, produce outputs.
    B_.SetInsertPoint(ev_done);
    s = B_.CreateLoad(i64_, startA, "start");
    Value* empty = B_.CreateICmpEQ(s, k, "win.empty");
    Value* count = B_.CreateAdd(B_.CreateSub(k, s), B_.getInt64(1), "count");
    std::vector<Slot> outs;
    for (size_t a = 0; a < na; ++a) {
      const BAgg& g = st.aggs[a];
      llvm::Type* ty = value_ty(g.in_type);
      bool fp = g.in_type == Type::F64;
      Value* result = nullptr;
      switch (g.fn) {
      case AggFn::Count: result = count; break;
      case AggFn::Sum:
      case AggFn::Mean: {
        Value* sum = B_.CreateLoad(ty, sumA[a]);
        sum = B_.CreateSelect(empty, Constant::getNullValue(ty), sum); // reset FP drift
        Value* x = xval[g.col];
        sum = fp ? B_.CreateFAdd(sum, x) : B_.CreateAdd(sum, x);
        B_.CreateStore(sum, sumA[a]);
        if (g.fn == AggFn::Sum) result = sum;
        else result = B_.CreateFDiv(fp ? sum : B_.CreateSIToFP(sum, f64_), B_.CreateSIToFP(count, f64_), "mean");
        break;
      }
      case AggFn::Min:
      case AggFn::Max: {
        bool is_min = g.fn == AggFn::Min;
        Value* x = xval[g.col];
        Value* hist = xhist[g.col];
        // Pop from the back while the back is no better than x.
        BasicBlock* pb_cond = block("win.dq.back.cond");
        BasicBlock* pb_check = block("win.dq.back.check");
        BasicBlock* pb_pop = block("win.dq.back.pop");
        BasicBlock* pb_done = block("win.dq.back.done");
        B_.CreateBr(pb_cond);
        B_.SetInsertPoint(pb_cond);
        Value* tl = B_.CreateLoad(i64_, tailA[a], "tail");
        Value* hd = B_.CreateLoad(i64_, headA[a], "head");
        B_.CreateCondBr(B_.CreateICmpSGT(tl, hd), pb_check, pb_done);
        B_.SetInsertPoint(pb_check);
        Value* tm1 = B_.CreateSub(tl, B_.getInt64(1));
        Value* xb = gep_load(ty, hist, gep_load(i64_, dq[a], tm1), "x.back");
        Value* pop = fp ? (is_min ? B_.CreateFCmpOGE(xb, x) : B_.CreateFCmpOLE(xb, x))
                        : (is_min ? B_.CreateICmpSGE(xb, x) : B_.CreateICmpSLE(xb, x));
        B_.CreateCondBr(pop, pb_pop, pb_done);
        B_.SetInsertPoint(pb_pop);
        B_.CreateStore(tm1, tailA[a]);
        B_.CreateBr(pb_cond);
        B_.SetInsertPoint(pb_done);
        tl = B_.CreateLoad(i64_, tailA[a], "tail");
        gep_store(k, dq[a], tl);
        B_.CreateStore(B_.CreateAdd(tl, B_.getInt64(1)), tailA[a]);
        // Pop stale indices from the front.
        BasicBlock* fr_cond = block("win.dq.front.cond");
        BasicBlock* fr_pop = block("win.dq.front.pop");
        BasicBlock* fr_done = block("win.dq.front.done");
        B_.CreateBr(fr_cond);
        B_.SetInsertPoint(fr_cond);
        hd = B_.CreateLoad(i64_, headA[a], "head");
        Value* front = gep_load(i64_, dq[a], hd, "front");
        B_.CreateCondBr(B_.CreateICmpSLT(front, s), fr_pop, fr_done);
        B_.SetInsertPoint(fr_pop);
        B_.CreateStore(B_.CreateAdd(hd, B_.getInt64(1)), headA[a]);
        B_.CreateBr(fr_cond);
        B_.SetInsertPoint(fr_done);
        result = gep_load(ty, hist, front, is_min ? "min" : "max");
        break;
      }
      }
      outs.push_back(Slot{g.out_type, result, -1});
    }
    B_.CreateStore(B_.CreateAdd(k, B_.getInt64(1)), kA);
    for (auto& o : outs) cur.push_back(o);
  }

  std::string cur_name(const BStage& st, int c) {
    // Name of column c of this stage's input == the window's output schema prefix.
    return st.out[c].name;
  }

  // Backward as-of join: merge with a forward-only cursor into the right
  // table; last[code] remembers the latest right row per symbol.
  void emit_asof(const BStage& st, std::vector<Slot>& cur) {
    Value* t = get(cur[st.ts_col]);
    Value* lk = get(cur[st.left_key]);
    const auto& rp = in_ptrs_[st.right_table];
    Value* nr = in_lens_[st.right_table];
    AllocaInst* rpA = alloca_init(i64_, B_.getInt64(0), "asof.rp");
    Value* last = add_scratch({8, ScratchSpec::Len::NSym, true, -1}, "asof.last");

    BasicBlock* adv_cond = block("asof.adv.cond");
    BasicBlock* adv_check = block("asof.adv.check");
    BasicBlock* adv_body = block("asof.adv.body");
    BasicBlock* adv_upd = block("asof.adv.update");
    BasicBlock* adv_next = block("asof.adv.next");
    BasicBlock* adv_done = block("asof.adv.done");
    B_.CreateBr(adv_cond);
    B_.SetInsertPoint(adv_cond);
    Value* r = B_.CreateLoad(i64_, rpA, "rp");
    B_.CreateCondBr(B_.CreateICmpSLT(r, nr), adv_check, adv_done);
    B_.SetInsertPoint(adv_check);
    Value* rts = gep_load(i64_, rp[st.right_ts], r, "rts");
    B_.CreateCondBr(B_.CreateICmpSLE(rts, t), adv_body, adv_done);
    B_.SetInsertPoint(adv_body);
    Value* rk = B_.CreateSExt(gep_load(i32_, rp[st.right_key], r, "rkey"), i64_);
    B_.CreateCondBr(B_.CreateICmpULT(rk, nsym_), adv_upd, adv_next);
    B_.SetInsertPoint(adv_upd);
    gep_store(r, last, rk);
    B_.CreateBr(adv_next);
    B_.SetInsertPoint(adv_next);
    B_.CreateStore(B_.CreateAdd(r, B_.getInt64(1)), rpA);
    B_.CreateBr(adv_cond);

    B_.SetInsertPoint(adv_done);
    Value* lkz = B_.CreateSExt(lk, i64_);
    Value* ok = B_.CreateICmpULT(lkz, nsym_);
    Value* j0 = gep_load(i64_, last, B_.CreateSelect(ok, lkz, B_.getInt64(0)));
    Value* j = B_.CreateSelect(ok, j0, B_.getInt64(uint64_t(-1)), "j");
    BasicBlock* gather = block("asof.gather");
    BasicBlock* merge = block("asof.merge");
    BasicBlock* from = B_.GetInsertBlock();
    B_.CreateCondBr(B_.CreateICmpSGE(j, B_.getInt64(0)), gather, merge);
    B_.SetInsertPoint(gather);
    std::vector<Value*> vals;
    for (const auto& p : st.payload) {
      Value* v = gep_load(storage_ty(p.in_type), rp[p.right_col], j);
      vals.push_back(p.in_type == Type::F64 ? v : B_.CreateSIToFP(v, f64_));
    }
    B_.CreateBr(merge);
    B_.SetInsertPoint(merge);
    Value* nan = ConstantFP::getQNaN(f64_);
    for (size_t p = 0; p < st.payload.size(); ++p) {
      PHINode* phi = B_.CreatePHI(f64_, 2, st.payload[p].name);
      phi->addIncoming(nan, from);
      phi->addIncoming(vals[p], gather);
      cur.push_back(Slot{Type::F64, phi, -1});
    }
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
