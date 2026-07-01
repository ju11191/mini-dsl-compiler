#include "datagen.h"

#include <cmath>

namespace tsq {

namespace {
uint64_t splitmix64(uint64_t& x) {
  uint64_t z = (x += 0x9e3779b97f4a7c15ULL);
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
  z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
  return z ^ (z >> 31);
}
uint64_t rotl(uint64_t x, int k) { return (x << k) | (x >> (64 - k)); }

// 2024-01-02 14:30:00 UTC (09:30 New York) in ns since the epoch.
constexpr int64_t kStartNs = 1704205800LL * 1000000000LL;

// Long-run mean price per symbol; AAPL hovers around 100 so `price > 100` is selective.
double base_price(int s) {
  static const double kBase[] = {100, 400, 140, 180, 500, 120, 250, 95};
  return s < 8 ? kBase[s] : 50.0 + 25.0 * s;
}

// Mean-reverting (Ornstein-Uhlenbeck style) step: stays within a few % of base.
double step(double px, double base, Rng& rng) {
  return px + 0.001 * (base - px) + base * 0.002 * (rng.uniform() - 0.5);
}
} // namespace

Rng::Rng(uint64_t seed) {
  for (auto& s : s_) s = splitmix64(seed);
}

uint64_t Rng::next() {
  uint64_t result = rotl(s_[1] * 5, 7) * 9;
  uint64_t t = s_[1] << 17;
  s_[2] ^= s_[0];
  s_[3] ^= s_[1];
  s_[1] ^= s_[2];
  s_[0] ^= s_[3];
  s_[2] ^= t;
  s_[3] = rotl(s_[3], 45);
  return result;
}

double Rng::uniform() { return double(next() >> 11) * 0x1.0p-53; }

int64_t Rng::range(int64_t lo, int64_t hi) { return lo + int64_t(next() % uint64_t(hi - lo + 1)); }

std::vector<std::string> default_symbols() {
  return {"AAPL", "MSFT", "GOOG", "AMZN", "META", "NVDA", "TSLA", "JPM"};
}

OwnedTable gen_trades(int64_t n, uint64_t seed, int nsym) {
  OwnedTable t({{"ts", Type::I64}, {"sym", Type::Sym}, {"price", Type::F64}, {"size", Type::I64}}, n);
  auto* ts = t.col<int64_t>("ts");
  auto* sym = t.col<int32_t>("sym");
  auto* price = t.col<double>("price");
  auto* size = t.col<int64_t>("size");
  Rng rng(seed);
  std::vector<double> px(nsym);
  for (int s = 0; s < nsym; ++s) px[s] = base_price(s);
  int64_t now = kStartNs;
  for (int64_t i = 0; i < n; ++i) {
    now += rng.range(0, 2000) * 1000; // 0..2ms gaps (duplicates possible), mean 1ms
    int s = int(rng.next() % uint64_t(nsym));
    px[s] = step(px[s], base_price(s), rng);
    ts[i] = now;
    sym[i] = s;
    price[i] = std::round(px[s] * 100.0) / 100.0;
    size[i] = rng.range(1, 1000);
  }
  return t;
}

OwnedTable gen_quotes(int64_t n, uint64_t seed, int nsym) {
  OwnedTable t({{"ts", Type::I64}, {"sym", Type::Sym}, {"bid", Type::F64}, {"ask", Type::F64}}, n);
  auto* ts = t.col<int64_t>("ts");
  auto* sym = t.col<int32_t>("sym");
  auto* bid = t.col<double>("bid");
  auto* ask = t.col<double>("ask");
  Rng rng(seed ^ 0x51ab1e5ULL);
  std::vector<double> mid(nsym);
  for (int s = 0; s < nsym; ++s) mid[s] = base_price(s);
  // Quotes start slightly before trades so most trades find a quote.
  int64_t now = kStartNs - 5'000'000;
  for (int64_t i = 0; i < n; ++i) {
    now += rng.range(0, 2000) * 1000;
    int s = int(rng.next() % uint64_t(nsym));
    mid[s] = step(mid[s], base_price(s), rng);
    double half = 0.01 * double(rng.range(1, 5));
    ts[i] = now;
    sym[i] = s;
    bid[i] = std::round((mid[s] - half) * 100.0) / 100.0;
    ask[i] = std::round((mid[s] + half) * 100.0) / 100.0;
  }
  return t;
}

} // namespace tsq
