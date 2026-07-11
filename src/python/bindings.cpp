// pybind11 bindings: tsq._core
#include "engine.h"
#include "parser.h"

#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <mutex>

namespace py = pybind11;
using namespace tsq;

namespace {

py::object g_compile_error;

Type parse_type(const std::string& s) {
  if (s == "i64" || s == "int64") return Type::I64;
  if (s == "f64" || s == "float64") return Type::F64;
  if (s == "sym" || s == "int32") return Type::Sym;
  throw py::value_error("unknown column type '" + s + "' (expected i64, f64 or sym)");
}

// numpy dtype -> column type: int64 -> i64, float64 -> f64, int32 -> sym codes.
Type dtype_type(const py::array& a, const std::string& where) {
  py::dtype dt = a.dtype();
  if (dt.is(py::dtype::of<int64_t>())) return Type::I64;
  if (dt.is(py::dtype::of<double>())) return Type::F64;
  if (dt.is(py::dtype::of<int32_t>())) return Type::Sym;
  if (dt.kind() == 'i' && dt.itemsize() == 8) return Type::I64;
  if (dt.kind() == 'f' && dt.itemsize() == 8) return Type::F64;
  if (dt.kind() == 'i' && dt.itemsize() == 4) return Type::Sym;
  throw py::type_error("column '" + where + "' has dtype " + std::string(py::str(dt)) +
                       "; expected int64 (i64), float64 (f64) or int32 (sym codes)");
}

[[noreturn]] void raise_compile_error(const CompileError& e, const std::string& text) {
  py::object err = g_compile_error(py::str(e.render(text)));
  err.attr("message") = e.message();
  err.attr("line") = e.loc().line;
  err.attr("col") = e.loc().col;
  err.attr("offset") = e.loc().offset;
  err.attr("length") = e.loc().len;
  PyErr_SetObject(g_compile_error.ptr(), err.ptr());
  throw py::error_already_set();
}

class PyQuery {
public:
  PyQuery(std::unique_ptr<CompiledQuery> q) : q_(std::move(q)) {}

  py::dict run(const py::dict& tables, const std::vector<std::string>& symbols, bool validate) {
    // Keep contiguous arrays alive for the duration of the run.
    std::vector<py::array> keep;
    NamedTables nt;
    for (const auto& bt : q_->plan().tables) {
      if (!tables.contains(bt.name)) throw py::key_error("missing input table '" + bt.name + "'");
      py::dict cols = tables[py::str(bt.name)].cast<py::dict>();
      NamedTable t;
      bool first = true;
      for (const auto& cd : bt.schema) {
        if (!cols.contains(cd.name))
          throw py::key_error("table '" + bt.name + "' is missing column '" + cd.name + "'");
        py::array a = py::array::ensure(cols[py::str(cd.name)], py::array::c_style);
        if (!a) throw py::type_error("column '" + bt.name + "." + cd.name + "' is not array-like");
        if (a.ndim() != 1)
          throw py::value_error("column '" + bt.name + "." + cd.name + "' must be 1-D");
        Type ty = dtype_type(a, bt.name + "." + cd.name);
        int64_t n = int64_t(a.shape(0));
        if (first)
          t.rows = n;
        else if (n != t.rows)
          throw py::value_error("table '" + bt.name + "': column '" + cd.name + "' has " +
                                std::to_string(n) + " rows, expected " + std::to_string(t.rows));
        first = false;
        t.cols[cd.name] = ColumnView{ty, a.data()};
        keep.push_back(std::move(a));
      }
      nt[bt.name] = std::move(t);
    }
    BoundInputs bi = q_->bind_inputs(nt, symbols, validate);
    Result r;
    {
      py::gil_scoped_release release;
      // Executables reuse scratch buffers, so concurrent runs of one query
      // (possible once the GIL is released) are serialized.
      std::lock_guard<std::mutex> lock(*mu_);
      r = q_->run(bi);
    }
    py::dict out;
    for (auto& c : r.cols) {
      size_t w = type_size(c.type);
      std::byte* p = c.data.release(); // ownership moves to the numpy array
      py::capsule owner(p, [](void* q) { std::free(q); });
      py::dtype dt = c.type == Type::I64   ? py::dtype::of<int64_t>()
                     : c.type == Type::F64 ? py::dtype::of<double>()
                                           : py::dtype::of<int32_t>();
      out[py::str(c.name)] = py::array(dt, {r.rows}, {ssize_t(w)}, p, owner);
    }
    return out;
  }

  std::string ir(bool optimized) const {
    return optimized ? q_->ir_optimized() : q_->ir_unoptimized();
  }

  py::dict stats() const {
    const auto& s = q_->stats();
    py::dict d;
    d["parse_bind_ms"] = s.parse_bind_ms;
    d["irgen_ms"] = s.irgen_ms;
    d["optimize_ms"] = s.optimize_ms;
    d["codegen_ms"] = s.codegen_ms;
    d["total_ms"] = s.total_ms();
    return d;
  }

  std::vector<std::pair<std::string, std::string>> output_schema() const {
    std::vector<std::pair<std::string, std::string>> v;
    for (const auto& c : q_->plan().output)
      v.emplace_back(c.name, type_name(c.type));
    return v;
  }

  std::vector<std::string> tables() const {
    std::vector<std::string> v;
    for (const auto& t : q_->plan().tables)
      v.push_back(t.name);
    return v;
  }

  std::string backend() const { return q_->backend() == Backend::Jit ? "jit" : "interp"; }

private:
  std::unique_ptr<CompiledQuery> q_;
  std::unique_ptr<std::mutex> mu_ = std::make_unique<std::mutex>();
};

} // namespace

PYBIND11_MODULE(_core, m) {
  m.doc() = "tsq native core: parser, binder, interpreter and LLVM JIT";

  static py::exception<CompileError> compile_error(m, "CompileError", PyExc_ValueError);
  g_compile_error = compile_error;
  py::register_exception<RuntimeError>(m, "DataError", PyExc_ValueError);

  py::class_<PyQuery>(m, "Query")
      .def("run", &PyQuery::run, py::arg("tables"), py::arg("symbols"), py::arg("validate") = true)
      .def("ir", &PyQuery::ir, py::arg("optimized") = true)
      .def_property_readonly("stats", &PyQuery::stats)
      .def_property_readonly("output_schema", &PyQuery::output_schema)
      .def_property_readonly("tables", &PyQuery::tables)
      .def_property_readonly("backend", &PyQuery::backend);

  m.def(
      "compile",
      [](const std::string& text,
         const std::map<std::string, std::vector<std::pair<std::string, std::string>>>& schema,
         const std::string& backend, int opt_level, bool window_direct_input,
         bool window_output_history) {
        Catalog cat;
        for (const auto& [tname, cols] : schema)
          for (const auto& [cname, ty] : cols)
            cat.tables[tname].push_back(ColumnDef{cname, parse_type(ty)});
        if (backend != "jit" && backend != "interp")
          throw py::value_error("backend must be 'jit' or 'interp'");
        CompileOptions opts;
        opts.opt_level = opt_level;
        opts.window_direct_input = window_direct_input;
        opts.window_output_history = window_output_history;
        try {
          return PyQuery(CompiledQuery::compile(
              text, cat, backend == "jit" ? Backend::Jit : Backend::Interp, opts));
        } catch (const CompileError& e) {
          raise_compile_error(e, text);
        }
      },
      py::arg("query"), py::arg("schema"), py::arg("backend") = "jit", py::arg("opt_level") = 2,
      py::arg("window_direct_input") = true, py::arg("window_output_history") = true);

  m.def(
      "parse",
      [](const std::string& text) {
        try {
          Query q = parse(text);
          std::vector<std::string> tables{q.source};
          for (const auto& s : q.stages)
            if (s.kind == Stage::Kind::AsofJoin) tables.push_back(s.table);
          return std::make_pair(to_string(q), tables);
        } catch (const CompileError& e) {
          raise_compile_error(e, text);
        }
      },
      py::arg("query"), "Parse only; returns (canonical text, referenced table names).");
}
