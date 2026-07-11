#include "interp.h"

#include "semantics.h"

#include <cmath>
#include <cstring>
#include <limits>

namespace tsq {

namespace {

// A column of an intermediate relation: either a view of an input column or
// a buffer owned by the interpreter (shared between relations that pass it through).
struct RCol {
  Type type;
  const void* data;
  std::shared_ptr<Buffer> owner;
};

struct Relation {
  int64_t rows = 0;
  std::vector<RCol> cols;
};

union Value {
  int64_t i; // i64, and sym codes widened to i64
  double f;
  bool b;
};

RCol new_col(Type t, int64_t rows) {
  auto buf = std::make_shared<Buffer>(size_t(std::max<int64_t>(rows, 1)) * type_size(t));
  return RCol{t, buf->data(), buf};
}

class Interp {
public:
  Interp(const BoundQuery& q, const BoundInputs& in) : q_(q), in_(in) {}

  Result run() {
    Relation r;
    const TableView& src = in_.tables[0];
    r.rows = src.rows;
    for (const auto& c : src.cols)
      r.cols.push_back(RCol{c.type, c.data, nullptr});
    for (const auto& st : q_.stages) {
      switch (st.kind) {
      case BStage::Kind::Filter:
        r = filter(st, r);
        break;
      case BStage::Kind::Project:
        r = project(st, r);
        break;
      case BStage::Kind::Window:
        r = window(st, r);
        break;
      case BStage::Kind::Asof:
        r = asof(st, r);
        break;
      }
    }
    // Hand over buffers the interpreter owns exclusively; copy the rest
    // (views of input columns, or buffers shared by two output columns).
    Result res;
    res.rows = r.rows;
    for (size_t c = 0; c < r.cols.size(); ++c) {
      RCol& rc = r.cols[c];
      Column col{q_.output[c].name, q_.output[c].type, Buffer()};
      if (rc.owner && rc.owner.use_count() == 1) {
        col.data = std::move(*rc.owner);
      } else {
        col.data = Buffer(size_t(std::max<int64_t>(r.rows, 1)) * type_size(rc.type));
        if (r.rows) std::memcpy(col.data.data(), rc.data, size_t(r.rows) * type_size(rc.type));
      }
      res.cols.push_back(std::move(col));
    }
    return res;
  }

private:
  const BoundQuery& q_;
  const BoundInputs& in_;

  Value eval(const BExpr& e, const Relation& r, int64_t row) const {
    Value v;
    switch (e.kind) {
    case BExpr::Kind::Col: {
      const RCol& c = r.cols[e.col];
      switch (c.type) {
      case Type::I64:
        v.i = static_cast<const int64_t*>(c.data)[row];
        break;
      case Type::F64:
        v.f = static_cast<const double*>(c.data)[row];
        break;
      case Type::Sym:
        v.i = static_cast<const int32_t*>(c.data)[row];
        break;
      case Type::Bool:
        v.b = false;
        break; // never stored
      }
      return v;
    }
    case BExpr::Kind::ConstI64:
      v.i = e.i;
      return v;
    case BExpr::Kind::ConstF64:
      v.f = e.f;
      return v;
    case BExpr::Kind::ConstBool:
      v.b = e.b;
      return v;
    case BExpr::Kind::SymParam:
      v.i = in_.params[e.param];
      return v;
    case BExpr::Kind::CastF64:
      v.f = double(eval(*e.kids[0], r, row).i);
      return v;
    case BExpr::Kind::Neg: {
      Value x = eval(*e.kids[0], r, row);
      if (e.type == Type::I64)
        v.i = sem::neg(x.i);
      else
        v.f = -x.f;
      return v;
    }
    case BExpr::Kind::Not:
      v.b = !eval(*e.kids[0], r, row).b;
      return v;
    case BExpr::Kind::Abs: {
      Value x = eval(*e.kids[0], r, row);
      if (e.type == Type::I64)
        v.i = sem::abs(x.i);
      else
        v.f = std::fabs(x.f);
      return v;
    }
    case BExpr::Kind::Sqrt:
      v.f = std::sqrt(eval(*e.kids[0], r, row).f);
      return v;
    case BExpr::Kind::Bin:
      return binary(e, r, row);
    }
    v.i = 0;
    return v;
  }

  Value binary(const BExpr& e, const Relation& r, int64_t row) const {
    Value v;
    // Logical operators short-circuit; expressions have no side effects, so
    // this is equivalent to the JIT's eager evaluation.
    if (e.op == BinOp::And) {
      v.b = eval(*e.kids[0], r, row).b && eval(*e.kids[1], r, row).b;
      return v;
    }
    if (e.op == BinOp::Or) {
      v.b = eval(*e.kids[0], r, row).b || eval(*e.kids[1], r, row).b;
      return v;
    }
    Value a = eval(*e.kids[0], r, row);
    Value b = eval(*e.kids[1], r, row);
    Type t = e.kids[0]->type; // operand type (both sides identical after binding)
    switch (e.op) {
    case BinOp::Add:
      if (t == Type::I64)
        v.i = sem::add(a.i, b.i);
      else
        v.f = a.f + b.f;
      return v;
    case BinOp::Sub:
      if (t == Type::I64)
        v.i = sem::sub(a.i, b.i);
      else
        v.f = a.f - b.f;
      return v;
    case BinOp::Mul:
      if (t == Type::I64)
        v.i = sem::mul(a.i, b.i);
      else
        v.f = a.f * b.f;
      return v;
    case BinOp::Div:
      v.f = a.f / b.f;
      return v;
    case BinOp::Eq:
      v.b = t == Type::F64 ? a.f == b.f : t == Type::Bool ? a.b == b.b : a.i == b.i;
      return v;
    case BinOp::Ne:
      v.b = t == Type::F64 ? a.f != b.f : t == Type::Bool ? a.b != b.b : a.i != b.i;
      return v;
    case BinOp::Lt:
      v.b = t == Type::F64 ? a.f < b.f : a.i < b.i;
      return v;
    case BinOp::Le:
      v.b = t == Type::F64 ? a.f <= b.f : a.i <= b.i;
      return v;
    case BinOp::Gt:
      v.b = t == Type::F64 ? a.f > b.f : a.i > b.i;
      return v;
    case BinOp::Ge:
      v.b = t == Type::F64 ? a.f >= b.f : a.i >= b.i;
      return v;
    case BinOp::And:
    case BinOp::Or:
      break; // handled above
    }
    v.i = 0;
    return v;
  }

  static void store(RCol& c, int64_t row, Value v) {
    switch (c.type) {
    case Type::I64:
      static_cast<int64_t*>(const_cast<void*>(c.data))[row] = v.i;
      break;
    case Type::F64:
      static_cast<double*>(const_cast<void*>(c.data))[row] = v.f;
      break;
    case Type::Sym:
      static_cast<int32_t*>(const_cast<void*>(c.data))[row] = int32_t(v.i);
      break;
    case Type::Bool:
      break;
    }
  }

  Relation filter(const BStage& st, const Relation& r) {
    std::vector<int64_t> sel;
    sel.reserve(size_t(r.rows));
    for (int64_t i = 0; i < r.rows; ++i)
      if (eval(*st.pred, r, i).b) sel.push_back(i);
    Relation out;
    out.rows = int64_t(sel.size());
    for (const auto& c : r.cols) {
      RCol nc = new_col(c.type, out.rows);
      size_t w = type_size(c.type);
      auto* dst = static_cast<std::byte*>(const_cast<void*>(nc.data));
      auto* src = static_cast<const std::byte*>(c.data);
      for (size_t k = 0; k < sel.size(); ++k)
        std::memcpy(dst + k * w, src + size_t(sel[k]) * w, w);
      out.cols.push_back(std::move(nc));
    }
    return out;
  }

  Relation project(const BStage& st, const Relation& r) {
    Relation out;
    out.rows = r.rows;
    for (const auto& e : st.exprs) {
      if (e->kind == BExpr::Kind::Col) { // pass-through: share the column
        out.cols.push_back(r.cols[e->col]);
        continue;
      }
      RCol nc = new_col(e->type, r.rows);
      for (int64_t i = 0; i < r.rows; ++i)
        store(nc, i, eval(*e, r, i));
      out.cols.push_back(std::move(nc));
    }
    return out;
  }

  // Trailing time window (ts - dur, ts], computed incrementally:
  // running sums for sum/mean, monotonic deques for min/max.
  Relation window(const BStage& st, const Relation& r) {
    Relation out = r; // pass-through columns
    const int64_t n = r.rows;
    const auto* ts = static_cast<const int64_t*>(r.cols[st.ts_col].data);
    size_t na = st.aggs.size();
    std::vector<RCol> outc;
    for (const auto& a : st.aggs)
      outc.push_back(new_col(a.out_type, n));
    std::vector<int64_t> isum(na, 0);
    std::vector<double> fsum(na, 0.0);
    std::vector<std::vector<int64_t>> dq(na);
    std::vector<int64_t> head(na, 0), tail(na, 0);
    for (size_t a = 0; a < na; ++a)
      if (st.aggs[a].fn == AggFn::Min || st.aggs[a].fn == AggFn::Max)
        dq[a].resize(size_t(std::max<int64_t>(n, 1)));

    auto xi = [&](size_t a, int64_t k) {
      return static_cast<const int64_t*>(r.cols[st.aggs[a].col].data)[k];
    };
    auto xf = [&](size_t a, int64_t k) {
      return static_cast<const double*>(r.cols[st.aggs[a].col].data)[k];
    };

    int64_t start = 0;
    for (int64_t k = 0; k < n; ++k) {
      const int64_t lo = sem::sat_sub(ts[k], st.dur_ns);
      // 1. evict rows that fell out of the window
      while (ts[start] <= lo) {
        for (size_t a = 0; a < na; ++a) {
          const BAgg& g = st.aggs[a];
          if (g.fn == AggFn::Sum || g.fn == AggFn::Mean) {
            if (g.in_type == Type::I64)
              isum[a] = sem::sub(isum[a], xi(a, start));
            else
              fsum[a] = fsum[a] - xf(a, start);
          }
        }
        ++start;
      }
      // 2. add row k and produce outputs
      const int64_t count = k - start + 1;
      for (size_t a = 0; a < na; ++a) {
        const BAgg& g = st.aggs[a];
        Value v;
        switch (g.fn) {
        case AggFn::Count:
          v.i = count;
          break;
        case AggFn::Sum:
        case AggFn::Mean:
          if (start == k) { // window was empty: reset to avoid FP drift
            isum[a] = 0;
            fsum[a] = 0.0;
          }
          if (g.in_type == Type::I64) {
            isum[a] = sem::add(isum[a], xi(a, k));
            if (g.fn == AggFn::Sum)
              v.i = isum[a];
            else
              v.f = double(isum[a]) / double(count);
          } else {
            fsum[a] = fsum[a] + xf(a, k);
            if (g.fn == AggFn::Sum)
              v.f = fsum[a];
            else
              v.f = fsum[a] / double(count);
          }
          break;
        case AggFn::Min:
        case AggFn::Max: {
          bool is_min = g.fn == AggFn::Min;
          int64_t* d = dq[a].data();
          int64_t& h = head[a];
          int64_t& t = tail[a];
          if (g.in_type == Type::I64) {
            int64_t x = xi(a, k);
            while (t > h && (is_min ? xi(a, d[t - 1]) >= x : xi(a, d[t - 1]) <= x))
              --t;
          } else {
            double x = xf(a, k);
            while (t > h && (is_min ? xf(a, d[t - 1]) >= x : xf(a, d[t - 1]) <= x))
              --t;
          }
          d[t++] = k;
          while (d[h] < start)
            ++h;
          if (g.in_type == Type::I64)
            v.i = xi(a, d[h]);
          else
            v.f = xf(a, d[h]);
          break;
        }
        }
        store(outc[a], k, v);
      }
    }
    for (auto& c : outc)
      out.cols.push_back(std::move(c));
    return out;
  }

  // Backward as-of join as a merge over the two ts-sorted inputs.
  Relation asof(const BStage& st, const Relation& r) {
    Relation out = r;
    const TableView& rt = in_.tables[st.right_table];
    const int64_t n = r.rows, nr = rt.rows, nsym = in_.nsym;
    const auto* lts = static_cast<const int64_t*>(r.cols[st.ts_col].data);
    const auto* lkey = static_cast<const int32_t*>(r.cols[st.left_key].data);
    const auto* rts = static_cast<const int64_t*>(rt.cols[st.right_ts].data);
    const auto* rkey = static_cast<const int32_t*>(rt.cols[st.right_key].data);
    std::vector<int64_t> last(size_t(std::max<int64_t>(nsym, 1)), -1);
    std::vector<RCol> outc;
    for (size_t p = 0; p < st.payload.size(); ++p)
      outc.push_back(new_col(Type::F64, n));
    int64_t rp = 0;
    for (int64_t i = 0; i < n; ++i) {
      const int64_t t = lts[i];
      while (rp < nr && rts[rp] <= t) {
        int32_t k = rkey[rp];
        if (k >= 0 && k < nsym) last[size_t(k)] = rp;
        ++rp;
      }
      int32_t k = lkey[i];
      int64_t j = (k >= 0 && k < nsym) ? last[size_t(k)] : -1;
      for (size_t p = 0; p < st.payload.size(); ++p) {
        const BPayload& pl = st.payload[p];
        double v = std::numeric_limits<double>::quiet_NaN();
        if (j >= 0) {
          const void* d = rt.cols[pl.right_col].data;
          v = pl.in_type == Type::F64 ? static_cast<const double*>(d)[j]
                                      : double(static_cast<const int64_t*>(d)[j]);
        }
        static_cast<double*>(const_cast<void*>(outc[p].data))[i] = v;
      }
    }
    for (auto& c : outc)
      out.cols.push_back(std::move(c));
    return out;
  }
};

} // namespace

Result interpret(const BoundQuery& q, const BoundInputs& in) {
  return Interp(q, in).run();
}

} // namespace tsq
