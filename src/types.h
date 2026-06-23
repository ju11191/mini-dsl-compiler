// Column types and schemas.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace tsq {

// i64: int64_t, f64: double, sym: int32_t code into the run's symbol dictionary.
// bool only exists inside expressions; it is never stored in a column.
enum class Type : uint8_t { I64, F64, Sym, Bool };

const char* type_name(Type t);
size_t type_size(Type t); // storage size in bytes (Bool: 0, not storable)

struct ColumnDef {
  std::string name;
  Type type;
  bool operator==(const ColumnDef&) const = default;
};
using Schema = std::vector<ColumnDef>;

// Index of `name` in `s`, or -1.
int find_column(const Schema& s, std::string_view name);
std::string schema_to_string(const Schema& s); // "ts:i64, sym:sym, price:f64"

// Table name -> schema. Supplied by the caller (or inferred from numpy dtypes).
struct Catalog {
  std::map<std::string, Schema, std::less<>> tables;
};

} // namespace tsq
