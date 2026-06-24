// Scalar semantics shared by the interpreter and the hand-written benchmark
// baselines. The JIT emits the equivalent LLVM instructions (see codegen.cpp).
#pragma once

#include <cstdint>
#include <limits>

namespace tsq::sem {

// i64 + - * wrap around (two's complement); done in uint64_t so C++ has no UB.
inline int64_t add(int64_t a, int64_t b) { return int64_t(uint64_t(a) + uint64_t(b)); }
inline int64_t sub(int64_t a, int64_t b) { return int64_t(uint64_t(a) - uint64_t(b)); }
inline int64_t mul(int64_t a, int64_t b) { return int64_t(uint64_t(a) * uint64_t(b)); }
inline int64_t neg(int64_t a) { return int64_t(0 - uint64_t(a)); }
inline int64_t abs(int64_t a) { return a < 0 ? neg(a) : a; }

// Saturating a - b: lower bound of a trailing window ending at ts = a.
inline int64_t sat_sub(int64_t a, int64_t b) {
  int64_t r;
  if (__builtin_sub_overflow(a, b, &r)) return b > 0 ? std::numeric_limits<int64_t>::min() : std::numeric_limits<int64_t>::max();
  return r;
}

} // namespace tsq::sem
