#include "engine.h"

#include "interp.h"
#include "parser.h"

#include <chrono>

namespace tsq {

namespace {

double ms_since(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

class InterpExecutable : public Executable {
public:
  explicit InterpExecutable(const BoundQuery& q) : q_(q) {}
  Result run(const BoundInputs& in) override { return interpret(q_, in); }

private:
  const BoundQuery& q_;
};

} // namespace

// Defined in jit.cpp.
std::unique_ptr<Executable> make_jit_executable(const BoundQuery& q, const CompileOptions& opts,
                                                CompileStats& stats, std::string& ir_unopt,
                                                std::string& ir_opt);

NamedTable named(const OwnedTable& t) {
  NamedTable nt;
  nt.rows = t.rows;
  for (size_t i = 0; i < t.schema.size(); ++i)
    nt.cols[t.schema[i].name] = ColumnView{t.schema[i].type, t.cols[i].data()};
  return nt;
}

std::unique_ptr<CompiledQuery> CompiledQuery::compile(std::string text, const Catalog& cat,
                                                      Backend backend, const CompileOptions& opts) {
  auto cq = std::unique_ptr<CompiledQuery>(new CompiledQuery());
  cq->text_ = std::move(text);
  cq->backend_ = backend;
  auto t0 = std::chrono::steady_clock::now();
  cq->plan_ = bind(parse(cq->text_), cat);
  cq->stats_.parse_bind_ms = ms_since(t0);
  if (backend == Backend::Interp)
    cq->exec_ = std::make_unique<InterpExecutable>(cq->plan_);
  else
    cq->exec_ = make_jit_executable(cq->plan_, opts, cq->stats_, cq->ir_unopt_, cq->ir_opt_);
  return cq;
}

BoundInputs CompiledQuery::bind_inputs(const NamedTables& tables,
                                       const std::vector<std::string>& symbols,
                                       bool validate) const {
  BoundInputs bi;
  bi.nsym = int64_t(symbols.size());
  for (const auto& lit : plan_.sym_literals) {
    int64_t code = -1;
    for (size_t i = 0; i < symbols.size(); ++i)
      if (symbols[i] == lit) code = int64_t(i);
    bi.params.push_back(code);
  }
  for (const auto& bt : plan_.tables) {
    auto it = tables.find(bt.name);
    if (it == tables.end()) throw RuntimeError("missing input table '" + bt.name + "'");
    const NamedTable& nt = it->second;
    if (nt.rows < 0) throw RuntimeError("table '" + bt.name + "' has a negative row count");
    TableView tv;
    tv.rows = nt.rows;
    for (const auto& cd : bt.schema) {
      auto ct = nt.cols.find(cd.name);
      if (ct == nt.cols.end())
        throw RuntimeError("table '" + bt.name + "' is missing column '" + cd.name + "'");
      if (ct->second.type != cd.type)
        throw RuntimeError("column '" + bt.name + "." + cd.name + "' has type " +
                           type_name(ct->second.type) + " but the query was compiled for " +
                           type_name(cd.type));
      if (!ct->second.data && nt.rows > 0)
        throw RuntimeError("column '" + bt.name + "." + cd.name + "' has no data");
      tv.cols.push_back(ct->second);
    }
    if (validate) {
      if (bt.needs_sorted_ts) {
        int ts = find_column(bt.schema, "ts");
        const auto* p = static_cast<const int64_t*>(tv.cols[ts].data);
        for (int64_t i = 1; i < tv.rows; ++i)
          if (p[i] < p[i - 1])
            throw RuntimeError("table '" + bt.name +
                               "': ts must be ascending for window/asof_join (row " +
                               std::to_string(i) + ")");
      }
      for (size_t c = 0; c < bt.schema.size(); ++c) {
        if (bt.schema[c].type != Type::Sym) continue;
        const auto* p = static_cast<const int32_t*>(tv.cols[c].data);
        for (int64_t i = 0; i < tv.rows; ++i)
          if (p[i] < 0 || p[i] >= bi.nsym)
            throw RuntimeError("table '" + bt.name + "': sym code " + std::to_string(p[i]) +
                               " in column '" + bt.schema[c].name +
                               "' is outside the symbol dictionary (size " +
                               std::to_string(bi.nsym) + ")");
      }
    }
    bi.tables.push_back(std::move(tv));
  }
  return bi;
}

} // namespace tsq
