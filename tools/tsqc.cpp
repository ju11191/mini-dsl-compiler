// tsqc: compile a query and run it on generated trades/quotes data.
//
//   tsqc 'trades | filter price > 100 | window 5m mean(price)'
//   tsqc --emit ir-opt 'trades | filter sym == "AAPL"'
//   tsqc --backend interp --rows 1000000 --time '...'
#include "datagen.h"
#include "engine.h"
#include "parser.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>

using namespace tsq;

namespace {

void usage() {
  std::cerr
      << "usage: tsqc [--backend jit|interp] [--rows N] [--seed S] [--head K] [--time]\n"
         "            [--emit ast|plan|ir|ir-opt] [-O0|-O2] '<query>'\n"
         "Runs the query on synthetic tables `trades` (ts, sym, price, size) and\n"
         "`quotes` (ts, sym, bid, ask) with symbols AAPL MSFT GOOG AMZN META NVDA TSLA JPM.\n";
}

void print_value(const Column& c, int64_t i, const std::vector<std::string>& syms) {
  switch (c.type) {
  case Type::I64:
    std::printf("%20lld", (long long)c.as<int64_t>()[i]);
    break;
  case Type::F64:
    std::printf("%14.4f", c.as<double>()[i]);
    break;
  case Type::Sym: {
    int32_t code = c.as<int32_t>()[i];
    std::printf("%6s", code >= 0 && size_t(code) < syms.size() ? syms[size_t(code)].c_str() : "?");
    break;
  }
  case Type::Bool:
    break;
  }
}

} // namespace

int main(int argc, char** argv) {
  Backend backend = Backend::Jit;
  int64_t rows = 100000;
  uint64_t seed = 1;
  int64_t head = 10;
  bool timing = false;
  std::string emit;
  CompileOptions opts;
  std::string query;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto need = [&]() -> std::string {
      if (i + 1 >= argc) {
        usage();
        std::exit(2);
      }
      return argv[++i];
    };
    if (a == "--backend") {
      std::string b = need();
      backend = b == "interp" ? Backend::Interp : Backend::Jit;
    } else if (a == "--rows")
      rows = std::stoll(need());
    else if (a == "--seed")
      seed = std::stoull(need());
    else if (a == "--head")
      head = std::stoll(need());
    else if (a == "--time")
      timing = true;
    else if (a == "--emit")
      emit = need();
    else if (a == "-O0")
      opts.opt_level = 0;
    else if (a == "-O2")
      opts.opt_level = 2;
    else if (a == "-h" || a == "--help") {
      usage();
      return 0;
    } else if (!a.empty() && a[0] == '-') {
      usage();
      return 2;
    } else
      query = a;
  }
  if (query.empty()) {
    usage();
    return 2;
  }

  Catalog cat;
  cat.tables["trades"] = {
      {"ts", Type::I64}, {"sym", Type::Sym}, {"price", Type::F64}, {"size", Type::I64}};
  cat.tables["quotes"] = {
      {"ts", Type::I64}, {"sym", Type::Sym}, {"bid", Type::F64}, {"ask", Type::F64}};

  try {
    if (emit == "ast") {
      std::cout << to_string(parse(query)) << "\n";
      return 0;
    }
    if (emit == "plan") {
      BoundQuery bq = bind(parse(query), cat);
      for (size_t s = 0; s < bq.stages.size(); ++s) {
        const BStage& st = bq.stages[s];
        const char* k = st.kind == BStage::Kind::Filter    ? "Filter"
                        : st.kind == BStage::Kind::Project ? "Project"
                        : st.kind == BStage::Kind::Window  ? "Window"
                                                           : "Asof";
        std::cout << s << ": " << k;
        if (st.pred) std::cout << " " << to_string(*st.pred, bq.stage_input(s), bq);
        std::cout << "\n   -> " << schema_to_string(st.out) << "\n";
      }
      return 0;
    }
    if (!emit.empty() && emit != "ir" && emit != "ir-opt") {
      usage();
      return 2;
    }
    if (!emit.empty()) backend = Backend::Jit;
    auto q = CompiledQuery::compile(query, cat, backend, opts);
    if (emit == "ir") {
      std::cout << q->ir_unoptimized();
      return 0;
    }
    if (emit == "ir-opt") {
      std::cout << q->ir_optimized();
      return 0;
    }

    auto symbols = default_symbols();
    OwnedTable trades = gen_trades(rows, seed);
    OwnedTable quotes = gen_quotes(rows, seed + 1);
    NamedTables tables{{"trades", named(trades)}, {"quotes", named(quotes)}};
    auto bi = q->bind_inputs(tables, symbols);
    auto t0 = std::chrono::steady_clock::now();
    Result r = q->run(bi);
    double run_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();

    for (const auto& c : r.cols) {
      int w = c.type == Type::I64 ? 20 : c.type == Type::F64 ? 14 : 6;
      std::printf("%*s ", w, c.name.c_str());
    }
    std::printf("\n");
    for (int64_t i = 0; i < std::min(head, r.rows); ++i) {
      for (const auto& c : r.cols) {
        print_value(c, i, symbols);
        std::printf(" ");
      }
      std::printf("\n");
    }
    std::printf("(%lld of %lld rows)\n", (long long)r.rows, (long long)rows);
    if (timing) {
      const auto& s = q->stats();
      std::printf(
          "backend=%s parse+bind=%.3fms irgen=%.3fms opt=%.3fms codegen=%.3fms run=%.3fms\n",
          backend == Backend::Jit ? "jit" : "interp", s.parse_bind_ms, s.irgen_ms, s.optimize_ms,
          s.codegen_ms, run_ms);
    }
  } catch (const CompileError& e) {
    std::cerr << e.render(query) << "\n";
    return 1;
  } catch (const std::exception& e) {
    std::cerr << "error: " << e.what() << "\n";
    return 1;
  }
  return 0;
}
