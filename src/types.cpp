#include "types.h"

namespace tsq {

const char* type_name(Type t) {
  switch (t) {
  case Type::I64:
    return "i64";
  case Type::F64:
    return "f64";
  case Type::Sym:
    return "sym";
  case Type::Bool:
    return "bool";
  }
  return "?";
}

size_t type_size(Type t) {
  switch (t) {
  case Type::I64:
    return 8;
  case Type::F64:
    return 8;
  case Type::Sym:
    return 4;
  case Type::Bool:
    return 0;
  }
  return 0;
}

int find_column(const Schema& s, std::string_view name) {
  for (size_t i = 0; i < s.size(); ++i)
    if (s[i].name == name) return int(i);
  return -1;
}

std::string schema_to_string(const Schema& s) {
  std::string out;
  for (size_t i = 0; i < s.size(); ++i) {
    if (i) out += ", ";
    out += s[i].name + ":" + type_name(s[i].type);
  }
  return out;
}

} // namespace tsq
