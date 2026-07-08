// Benchmark: interpreter vs JIT vs hand-written C++ for each query type.
//
//   bench_queries [--rows N] [--reps R] [--only name,name] [--no-interp] [--window-direct 0|1]
//
// Rows named "<query>/&" rerun <query> with a hand-written variant that uses a
// non-short-circuit `&` in the predicate (one branch per row, like the JIT).
//
// All three produce the same output columns; each run's output is hashed and
// the hashes must match across the three implementations.
#include "datagen.h"
#include "engine.h"
#include "handwritten.h"

#include <sys/resource.h>
#if defined(__APPLE__)
#include <pthread/qos.h>
#endif

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

using namespace tsq;
using Clock = std::chrono::steady_clock;

namespace {

double ms(Clock::time_point a, Clock::time_point b) { return std::chrono::duration<double, std::milli>(b - a).count(); }

double median(std::vector<double> v) {
  std::sort(v.begin(), v.end());
  size_t n = v.size();
  return n % 2 ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

uint64_t hash_bytes(const void* p, size_t bytes, uint64_t h) {
  const auto* b = static_cast<const unsigned char*>(p);
  size_t words = bytes / 8;
  for (size_t i = 0; i < words; ++i) {
    uint64_t w;
    std::memcpy(&w, b + i * 8, 8);
    h = (h ^ w) * 0x100000001b3ULL;
    h ^= h >> 29;
  }
  for (size_t i = words * 8; i < bytes; ++i) h = (h ^ b[i]) * 0x100000001b3ULL;
  return h;
}

// Hash of an output (rows + every column's first `rows` values). NaNs are
// canonicalized so different NaN payloads still compare equal.
uint64_t hash_output(const Schema& schema, void* const* cols, int64_t rows) {
  uint64_t h = 0xcbf29ce484222325ULL ^ uint64_t(rows);
  for (size_t c = 0; c < schema.size(); ++c) {
    if (schema[c].type == Type::F64) {
      auto* d = static_cast<double*>(cols[c]);
      for (int64_t i = 0; i < rows; ++i)
        if (d[i] != d[i]) d[i] = std::numeric_limits<double>::quiet_NaN();
    }
    h = hash_bytes(cols[c], size_t(rows) * type_size(schema[c].type), h);
  }
  return h;
}

long peak_rss_mb() {
  struct rusage ru;
  getrusage(RUSAGE_SELF, &ru);
  return long(ru.ru_maxrss / (1024 * 1024)); // macOS reports bytes
}

struct BenchQuery {
  std::string name;
  std::string text;
  std::function<int64_t(const hw::In&, void* const*, hw::Scratch&)> hand;
};

} // namespace

int main(int argc, char** argv) {
#if defined(__APPLE__)
  // Ask for the highest QoS so the scheduler prefers performance cores
  // (Apple silicon mixes P- and E-cores; E-cores are several times slower).
  pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
  int64_t rows = 50'000'000;
  int reps = 5;
  bool run_interp = true;
  bool interp_only = false; // time only the interpreter (JIT/hand run once, for the output hash)
  std::string only;
  CompileOptions copts;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--rows" && i + 1 < argc) rows = std::stoll(argv[++i]);
    else if (a == "--reps" && i + 1 < argc) reps = std::stoi(argv[++i]);
    else if (a == "--only" && i + 1 < argc) only = argv[++i];
    else if (a == "--no-interp") run_interp = false;
    else if (a == "--interp-only") interp_only = true;
    else if (a == "--window-direct" && i + 1 < argc) copts.window_direct_input = std::stoi(argv[++i]) != 0;
    else if (a == "--window-outhist" && i + 1 < argc) copts.window_output_history = std::stoi(argv[++i]) != 0;
    else {
      std::fprintf(stderr, "usage: bench_queries [--rows N] [--reps R] [--only a,b] [--no-interp | --interp-only] [--window-direct 0|1] [--window-outhist 0|1]\n");
      return 2;
    }
  }

  std::vector<BenchQuery> queries = {
      {"filter", R"(trades | filter price > 100 and sym == "AAPL")",
       [](const hw::In& in, void* const* o, hw::Scratch&) { return hw::filter(in, o); }},
      {"filter/&", R"(trades | filter price > 100 and sym == "AAPL")",
       [](const hw::In& in, void* const* o, hw::Scratch&) { return hw::filter_bitand(in, o); }},
      {"filter_wide", R"(trades | filter price > 100 | select ts, sym, price)",
       [](const hw::In& in, void* const* o, hw::Scratch&) { return hw::filter_wide(in, o); }},
      {"filter_project", R"(trades | filter price > 100 and size < 500 | select ts, notional = price * size)",
       [](const hw::In& in, void* const* o, hw::Scratch&) { return hw::filter_project(in, o); }},
      {"window", R"(trades | window 5m sum(size), mean(price), min(price), max(price))",
       [](const hw::In& in, void* const* o, hw::Scratch& s) { return hw::window(in, o, s); }},
      {"asof", R"(trades | asof_join quotes on sym)",
       [](const hw::In& in, void* const* o, hw::Scratch& s) { return hw::asof(in, o, s); }},
      {"combined", R"(trades | filter price > 100 and sym == "AAPL" | window 5m mean(price) | asof_join quotes on sym)",
       [](const hw::In& in, void* const* o, hw::Scratch& s) { return hw::combined(in, o, s); }},
      {"combined/&", R"(trades | filter price > 100 and sym == "AAPL" | window 5m mean(price) | asof_join quotes on sym)",
       [](const hw::In& in, void* const* o, hw::Scratch& s) { return hw::combined_bitand(in, o, s); }},
  };

  std::printf("# tsq benchmark: rows=%lld reps=%d window_direct_input=%d window_output_history=%d mode=%s\n",
              (long long)rows, reps, int(copts.window_direct_input), int(copts.window_output_history),
              interp_only ? "interp-only" : run_interp ? "all" : "native-only");
  auto selected = [&](const std::string& name) {
    return only.empty() || ("," + only + ",").find("," + name + ",") != std::string::npos;
  };
  // quotes are only generated when a selected query joins them (saves 1.4 GB at 50M rows).
  bool need_quotes = false;
  for (const auto& bq : queries)
    if (selected(bq.name) && bq.text.find("quotes") != std::string::npos) need_quotes = true;
  auto t0 = Clock::now();
  auto symbols = default_symbols();
  OwnedTable trades = gen_trades(rows, 1);
  OwnedTable quotes = gen_quotes(need_quotes ? rows : 0, 2);
  std::printf("# generated trades+quotes in %.0f ms\n", ms(t0, Clock::now()));
  NamedTables tables{{"trades", named(trades)}, {"quotes", named(quotes)}};

  hw::In hin{trades.col<int64_t>("ts"), trades.col<int32_t>("sym"), trades.col<double>("price"),
             trades.col<int64_t>("size"), rows, quotes.col<int64_t>("ts"), quotes.col<int32_t>("sym"),
             quotes.col<double>("bid"), quotes.col<double>("ask"), quotes.rows, 0, int64_t(symbols.size())};
  hw::Scratch hscratch;

  Catalog cat;
  cat.tables["trades"] = trades.schema;
  cat.tables["quotes"] = quotes.schema;

  std::printf("# times in ms; med = median, min = minimum over reps; ratios use medians (and mins)\n");
  std::printf("%-15s %10s %10s %9s %9s %9s %9s %9s %8s %8s %8s %8s %8s %8s %8s %8s %6s\n", "query", "out_rows",
              "interp", "jit_med", "jit_min", "hand_med", "hand_min", "jit_cold", "int/jit", "jit/hand", "(min)",
              "c_parse", "c_irgen", "c_opt", "c_mc", "c_total", "match");
  bool all_match = true;
  for (const auto& bq : queries) {
    if (!selected(bq.name)) continue;

    // Compile time: median of 5 fresh compiles (each creates its own LLJIT).
    std::vector<double> cp, ci, co, cm, ct;
    std::unique_ptr<CompiledQuery> jq;
    for (int r = 0; r < 5; ++r) {
      CompileOptions o = copts;
      o.keep_ir = false;
      auto a = Clock::now();
      jq = CompiledQuery::compile(bq.text, cat, Backend::Jit, o);
      auto b = Clock::now();
      const auto& s = jq->stats();
      cp.push_back(s.parse_bind_ms);
      ci.push_back(s.irgen_ms);
      co.push_back(s.optimize_ms);
      cm.push_back(s.codegen_ms);
      ct.push_back(ms(a, b));
    }
    BoundInputs bi = jq->bind_inputs(tables, symbols, /*validate=*/true);
    const Schema out_schema = jq->plan().output;

    // JIT cold: one run that allocates fresh output buffers (page faults included).
    double tjc = 0;
    if (!interp_only) {
      auto a = Clock::now();
      Result res = jq->run(bi);
      tjc = ms(a, Clock::now());
    }

    // Shared output buffers for the JIT and hand-written runs, pre-faulted.
    std::vector<Buffer> outb;
    std::vector<void*> outp;
    for (const auto& c : out_schema) {
      outb.emplace_back(size_t(rows) * type_size(c.type));
      outp.push_back(outb.back().data());
      std::memset(outp.back(), 0, outb.back().bytes());
    }

    // JIT and hand-written runs are interleaved so both see the same machine
    // conditions; rep 0 is a warm-up and is also the one whose output is hashed.
    std::vector<double> tj, th;
    int64_t jrows = 0, hrows = 0;
    uint64_t hj = 0, hh = 0;
    for (int r = 0; r < (interp_only ? 1 : reps + 1); ++r) {
      auto a = Clock::now();
      jrows = jq->run_raw(bi, outp.data());
      auto b = Clock::now();
      if (r == 0) hj = hash_output(out_schema, outp.data(), jrows);
      else tj.push_back(ms(a, b));
      a = Clock::now();
      hrows = bq.hand(hin, outp.data(), hscratch);
      b = Clock::now();
      if (r == 0) hh = hash_output(out_schema, outp.data(), hrows);
      else th.push_back(ms(a, b));
    }
    outb.clear();
    outp.clear();
    jq.reset(); // free JIT scratch before the (memory-hungry) interpreter runs
    hscratch = hw::Scratch{};

    // Interpreter (separate phase to bound peak memory at 50M rows).
    std::vector<double> ti;
    uint64_t hi = hj;
    int64_t irows = jrows;
    if (run_interp) {
      auto iq = CompiledQuery::compile(bq.text, cat, Backend::Interp);
      for (int r = 0; r < std::max(1, (reps + 1) / 2); ++r) {
        auto a = Clock::now();
        Result res = iq->run(bi);
        auto b = Clock::now();
        ti.push_back(ms(a, b));
        if (r == 0) {
          std::vector<void*> p;
          for (auto& c : res.cols) p.push_back(c.data.data());
          irows = res.rows;
          hi = hash_output(out_schema, p.data(), res.rows);
        }
      }
    }
    bool match = hj == hh && hj == hi && jrows == hrows && jrows == irows;
    all_match &= match;
    if (interp_only) tj = th = {0.0};
    double mi = run_interp ? median(ti) : 0, mj = median(tj), mh = median(th);
    double nj = *std::min_element(tj.begin(), tj.end()), nh = *std::min_element(th.begin(), th.end());
    std::printf("%-15s %10lld %10.1f %9.1f %9.1f %9.1f %9.1f %9.1f %8.2f %8.3f %8.3f %8.3f %8.3f %8.2f %8.2f %8.2f %6s\n",
                bq.name.c_str(), (long long)jrows, mi, mj, nj, mh, nh, tjc, run_interp ? mi / mj : 0.0,
                mj / mh, nj / nh, median(cp), median(ci), median(co), median(cm), median(ct), match ? "yes" : "NO");
    std::ostringstream raw;
    raw << "#raw " << bq.name << " jit_ms=";
    for (double v : tj) raw << v << ",";
    raw << " hand_ms=";
    for (double v : th) raw << v << ",";
    raw << " jit_cold_ms=" << tjc;
    raw << " interp_ms=";
    for (double v : ti) raw << v << ",";
    raw << " compile_total_ms=";
    for (double v : ct) raw << v << ",";
    std::printf("%s\n", raw.str().c_str());
    std::fflush(stdout);
  }
  std::printf("# peak RSS %ld MB; all outputs match: %s\n", peak_rss_mb(), all_match ? "yes" : "NO");
  return all_match ? 0 : 1;
}
