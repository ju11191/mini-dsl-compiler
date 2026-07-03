#include "handwritten.h"

#include <algorithm>
#include <limits>

namespace hw {

namespace {
constexpr int64_t kWin = 300'000'000'000LL; // 5m in ns
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

inline int64_t lower_bound_ts(int64_t t) {
  int64_t r;
  return __builtin_sub_overflow(t, kWin, &r) ? std::numeric_limits<int64_t>::min() : r;
}
} // namespace

int64_t filter(const In& in, void* const* out) {
  auto* ots = static_cast<int64_t*>(out[0]);
  auto* osym = static_cast<int32_t*>(out[1]);
  auto* opx = static_cast<double*>(out[2]);
  auto* osz = static_cast<int64_t*>(out[3]);
  int64_t o = 0;
  for (int64_t i = 0; i < in.n; ++i) {
    if (in.price[i] > 100.0 && in.sym[i] == in.aapl) {
      ots[o] = in.ts[i];
      osym[o] = in.sym[i];
      opx[o] = in.price[i];
      osz[o] = in.size[i];
      ++o;
    }
  }
  return o;
}

int64_t filter_wide(const In& in, void* const* out) {
  auto* ots = static_cast<int64_t*>(out[0]);
  auto* osym = static_cast<int32_t*>(out[1]);
  auto* opx = static_cast<double*>(out[2]);
  int64_t o = 0;
  for (int64_t i = 0; i < in.n; ++i) {
    if (in.price[i] > 100.0) {
      ots[o] = in.ts[i];
      osym[o] = in.sym[i];
      opx[o] = in.price[i];
      ++o;
    }
  }
  return o;
}

int64_t filter_project(const In& in, void* const* out) {
  auto* ots = static_cast<int64_t*>(out[0]);
  auto* onot = static_cast<double*>(out[1]);
  int64_t o = 0;
  for (int64_t i = 0; i < in.n; ++i) {
    if (in.price[i] > 100.0 && in.size[i] < 500) {
      ots[o] = in.ts[i];
      onot[o] = in.price[i] * double(in.size[i]);
      ++o;
    }
  }
  return o;
}

int64_t window(const In& in, void* const* out, Scratch& s) {
  const int64_t n = in.n;
  auto* ots = static_cast<int64_t*>(out[0]);
  auto* osym = static_cast<int32_t*>(out[1]);
  auto* opx = static_cast<double*>(out[2]);
  auto* osz = static_cast<int64_t*>(out[3]);
  auto* osum = static_cast<int64_t*>(out[4]);
  auto* omean = static_cast<double*>(out[5]);
  auto* omin = static_cast<double*>(out[6]);
  auto* omax = static_cast<double*>(out[7]);
  s.dq_min.resize(size_t(std::max<int64_t>(n, 1)));
  s.dq_max.resize(size_t(std::max<int64_t>(n, 1)));
  int64_t* dmin = s.dq_min.data();
  int64_t* dmax = s.dq_max.data();
  int64_t hmin = 0, tmin = 0, hmax = 0, tmax = 0;
  int64_t isum = 0;
  double fsum = 0.0;
  int64_t start = 0;
  const int64_t* ts = in.ts;
  const double* px = in.price;
  const int64_t* sz = in.size;
  for (int64_t k = 0; k < n; ++k) {
    const int64_t lo = lower_bound_ts(ts[k]);
    while (ts[start] <= lo) {
      isum -= sz[start];
      fsum = fsum - px[start];
      ++start;
    }
    if (start == k) {
      isum = 0;
      fsum = 0.0;
    }
    const double x = px[k];
    isum += sz[k];
    fsum = fsum + x;
    while (tmin > hmin && px[dmin[tmin - 1]] >= x) --tmin;
    dmin[tmin++] = k;
    while (dmin[hmin] < start) ++hmin;
    while (tmax > hmax && px[dmax[tmax - 1]] <= x) --tmax;
    dmax[tmax++] = k;
    while (dmax[hmax] < start) ++hmax;
    ots[k] = ts[k];
    osym[k] = in.sym[k];
    opx[k] = x;
    osz[k] = sz[k];
    osum[k] = isum;
    omean[k] = fsum / double(k - start + 1);
    omin[k] = px[dmin[hmin]];
    omax[k] = px[dmax[hmax]];
  }
  return n;
}

int64_t asof(const In& in, void* const* out, Scratch& s) {
  auto* ots = static_cast<int64_t*>(out[0]);
  auto* osym = static_cast<int32_t*>(out[1]);
  auto* opx = static_cast<double*>(out[2]);
  auto* osz = static_cast<int64_t*>(out[3]);
  auto* obid = static_cast<double*>(out[4]);
  auto* oask = static_cast<double*>(out[5]);
  s.last.assign(size_t(std::max<int64_t>(in.nsym, 1)), -1);
  int64_t* last = s.last.data();
  int64_t rp = 0;
  for (int64_t i = 0; i < in.n; ++i) {
    const int64_t t = in.ts[i];
    while (rp < in.nq && in.qts[rp] <= t) {
      last[in.qsym[rp]] = rp;
      ++rp;
    }
    const int64_t j = last[in.sym[i]];
    ots[i] = t;
    osym[i] = in.sym[i];
    opx[i] = in.price[i];
    osz[i] = in.size[i];
    obid[i] = j >= 0 ? in.bid[j] : kNaN;
    oask[i] = j >= 0 ? in.ask[j] : kNaN;
  }
  return in.n;
}

int64_t combined(const In& in, void* const* out, Scratch& s) {
  auto* ots = static_cast<int64_t*>(out[0]);
  auto* osym = static_cast<int32_t*>(out[1]);
  auto* opx = static_cast<double*>(out[2]);
  auto* osz = static_cast<int64_t*>(out[3]);
  auto* omean = static_cast<double*>(out[4]);
  auto* obid = static_cast<double*>(out[5]);
  auto* oask = static_cast<double*>(out[6]);
  s.last.assign(size_t(std::max<int64_t>(in.nsym, 1)), -1);
  int64_t* last = s.last.data();
  int64_t rp = 0, o = 0, start = 0;
  double fsum = 0.0;
  for (int64_t i = 0; i < in.n; ++i) {
    if (!(in.price[i] > 100.0 && in.sym[i] == in.aapl)) continue;
    const int64_t t = in.ts[i];
    const double x = in.price[i];
    // The output columns double as the window's history (they hold exactly
    // the filtered stream).
    ots[o] = t;
    opx[o] = x;
    const int64_t lo = lower_bound_ts(t);
    while (ots[start] <= lo) {
      fsum = fsum - opx[start];
      ++start;
    }
    if (start == o) fsum = 0.0;
    fsum = fsum + x;
    omean[o] = fsum / double(o - start + 1);
    while (rp < in.nq && in.qts[rp] <= t) {
      last[in.qsym[rp]] = rp;
      ++rp;
    }
    const int64_t j = last[in.aapl];
    osym[o] = in.sym[i];
    osz[o] = in.size[i];
    obid[o] = j >= 0 ? in.bid[j] : kNaN;
    oask[o] = j >= 0 ? in.ask[j] : kNaN;
    ++o;
  }
  return o;
}

} // namespace hw
