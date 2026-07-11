// Columnar data containers shared by the interpreter, the JIT executor,
// the data generator and the Python bindings.
#pragma once

#include "types.h"

#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <vector>

namespace tsq {

// Owned, uninitialized, 64-byte aligned heap buffer (malloc-compatible free).
class Buffer {
public:
  Buffer() = default;
  explicit Buffer(size_t bytes) : bytes_(bytes) {
    if (bytes == 0) return;
    void* p = nullptr;
    if (posix_memalign(&p, 64, bytes) != 0) throw std::bad_alloc();
    p_.reset(static_cast<std::byte*>(p));
  }
  std::byte* data() const { return p_.get(); }
  size_t bytes() const { return bytes_; }
  template <class T> T* as() const { return reinterpret_cast<T*>(p_.get()); }
  // Give up ownership (caller must free()).
  std::byte* release() {
    bytes_ = 0;
    return p_.release();
  }

private:
  struct Free {
    void operator()(std::byte* p) const { std::free(p); }
  };
  std::unique_ptr<std::byte, Free> p_;
  size_t bytes_ = 0;
};

// Non-owning view of one input column.
struct ColumnView {
  Type type = Type::I64;
  const void* data = nullptr;
};

// Non-owning view of one input table; columns follow the bound schema order.
struct TableView {
  int64_t rows = 0;
  std::vector<ColumnView> cols;
};

// Inputs resolved against a BoundQuery: tables[i] corresponds to BoundQuery::tables[i].
struct BoundInputs {
  std::vector<TableView> tables;
  std::vector<int64_t> params; // codes of BoundQuery::sym_literals (or -1)
  int64_t nsym = 0;            // size of the symbol dictionary
};

// One owned output column.
struct Column {
  std::string name;
  Type type;
  Buffer data;
  template <class T> const T* as() const { return data.as<T>(); }
};

struct Result {
  int64_t rows = 0;
  std::vector<Column> cols;
  const Column& col(const std::string& name) const {
    for (const auto& c : cols)
      if (c.name == name) return c;
    throw std::out_of_range("no column " + name);
  }
};

// An owned table (used by the data generator and tests).
struct OwnedTable {
  Schema schema;
  int64_t rows = 0;
  std::vector<Buffer> cols;

  OwnedTable() = default;
  OwnedTable(Schema s, int64_t n) : schema(std::move(s)), rows(n) {
    for (const auto& c : schema)
      cols.emplace_back(size_t(n) * type_size(c.type));
  }
  template <class T> T* col(const std::string& name) {
    int i = find_column(schema, name);
    if (i < 0) throw std::out_of_range("no column " + name);
    return cols[i].as<T>();
  }
  TableView view() const {
    TableView v;
    v.rows = rows;
    for (size_t i = 0; i < schema.size(); ++i)
      v.cols.push_back(ColumnView{schema[i].type, cols[i].data()});
    return v;
  }
};

} // namespace tsq
