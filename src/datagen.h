// Deterministic synthetic trades/quotes generator (C++ side; used by tsqc,
// the C++ tests and the benchmarks). The Python package has its own numpy
// generator with the same shape.
#pragma once

#include "table.h"

#include <string>
#include <vector>

namespace tsq {

std::vector<std::string> default_symbols(); // AAPL, MSFT, ... (8 tickers)

// trades: ts:i64 (ns, ascending), sym:sym, price:f64 (mean-reverting per symbol), size:i64
// (1..1000)
OwnedTable gen_trades(int64_t n, uint64_t seed, int nsym = 8);
// quotes: ts:i64 (ns, ascending), sym:sym, bid:f64, ask:f64
OwnedTable gen_quotes(int64_t n, uint64_t seed, int nsym = 8);

// Small fast PRNG (xoshiro256**), seeded with splitmix64.
class Rng {
public:
  explicit Rng(uint64_t seed);
  uint64_t next();
  double uniform();                      // [0, 1)
  int64_t range(int64_t lo, int64_t hi); // [lo, hi]
private:
  uint64_t s_[4];
};

} // namespace tsq
