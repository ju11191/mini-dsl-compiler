#include "diagnostics.h"

#include <algorithm>
#include <sstream>
#include <vector>

namespace tsq {

std::string CompileError::render(std::string_view src) const {
  // Find the line containing loc_.offset.
  size_t off = std::min<size_t>(loc_.offset, src.size());
  size_t begin = src.rfind('\n', off == 0 ? 0 : off - 1);
  begin = (begin == std::string_view::npos || off == 0) ? 0 : begin + 1;
  if (off > 0 && src[off - 1] == '\n') begin = off;
  size_t end = src.find('\n', off);
  if (end == std::string_view::npos) end = src.size();
  std::string_view line = src.substr(begin, end - begin);

  std::ostringstream os;
  os << "error: " << msg_ << "\n";
  os << "  --> " << loc_.line << ":" << loc_.col << "\n";
  std::string gutter = std::to_string(loc_.line);
  os << "  " << std::string(gutter.size(), ' ') << " |\n";
  os << "  " << gutter << " | " << line << "\n";
  size_t caret_col = off - begin;
  size_t len = std::max<size_t>(
      1, std::min<size_t>(loc_.len, line.size() > caret_col ? line.size() - caret_col : 1));
  os << "  " << std::string(gutter.size(), ' ') << " | " << std::string(caret_col, ' ')
     << std::string(len, '^');
  return os.str();
}

size_t edit_distance(std::string_view a, std::string_view b) {
  std::vector<size_t> prev(b.size() + 1), cur(b.size() + 1);
  for (size_t j = 0; j <= b.size(); ++j)
    prev[j] = j;
  for (size_t i = 1; i <= a.size(); ++i) {
    cur[0] = i;
    for (size_t j = 1; j <= b.size(); ++j) {
      size_t sub = prev[j - 1] + (a[i - 1] == b[j - 1] ? 0 : 1);
      cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, sub});
    }
    std::swap(prev, cur);
  }
  return prev[b.size()];
}

} // namespace tsq
