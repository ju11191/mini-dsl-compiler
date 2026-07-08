// Hand-written C++ versions of the benchmark queries: what a competent
// engineer would write for each one, single-threaded. They write into the
// same output layout as the JIT so the results can be compared bit-for-bit.
#pragma once

#include <cstdint>
#include <vector>

namespace hw {

struct In {
  const int64_t* ts;
  const int32_t* sym;
  const double* price;
  const int64_t* size;
  int64_t n;
  const int64_t* qts;
  const int32_t* qsym;
  const double* bid;
  const double* ask;
  int64_t nq;
  int32_t aapl; // code of "AAPL"
  int64_t nsym;
};

// Scratch reused across runs (like the JIT's scratch buffers).
struct Scratch {
  std::vector<int64_t> dq_min, dq_max, last;
};

// trades | filter price > 100 and sym == "AAPL"            -> ts, sym, price, size
int64_t filter(const In& in, void* const* out);
// Same query, written with a non-short-circuit `&` so there is one branch per row.
int64_t filter_bitand(const In& in, void* const* out);
// trades | filter price > 100 | select ts, sym, price        -> ts, sym, price
int64_t filter_wide(const In& in, void* const* out);
// trades | filter price > 100 and size < 500 | select ts, notional = price * size
int64_t filter_project(const In& in, void* const* out);
// trades | window 5m sum(size), mean(price), min(price), max(price)
int64_t window(const In& in, void* const* out, Scratch& s);
// trades | asof_join quotes on sym                           -> ts, sym, price, size, bid, ask
int64_t asof(const In& in, void* const* out, Scratch& s);
// trades | filter price > 100 and sym == "AAPL" | window 5m mean(price) | asof_join quotes on sym
int64_t combined(const In& in, void* const* out, Scratch& s);
// Same query with a non-short-circuit `&` in the predicate.
int64_t combined_bitand(const In& in, void* const* out, Scratch& s);

} // namespace hw
