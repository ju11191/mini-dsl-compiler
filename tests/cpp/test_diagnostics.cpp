#include "diagnostics.h"

#include <gtest/gtest.h>

using namespace tsq;

TEST(Diagnostics, EditDistance) {
  EXPECT_EQ(edit_distance("price", "prise"), 1u);
  EXPECT_EQ(edit_distance("", "abc"), 3u);
  EXPECT_EQ(edit_distance("kitten", "sitting"), 3u);
}

TEST(Diagnostics, RenderUnderlinesSpan) {
  std::string src = "trades | filter prise > 100";
  CompileError e("unknown column 'prise'", SourceLoc{16, 1, 17, 5});
  std::string r = e.render(src);
  EXPECT_NE(r.find("error: unknown column 'prise'"), std::string::npos);
  EXPECT_NE(r.find("--> 1:17"), std::string::npos);
  EXPECT_NE(r.find("1 | trades | filter prise > 100"), std::string::npos);
  EXPECT_NE(r.find("  |                 ^^^^^"), std::string::npos) << r;
}

TEST(Diagnostics, RenderSecondLine) {
  std::string src = "trades\n| filter x";
  CompileError e("bad", SourceLoc{16, 2, 10, 1});
  std::string r = e.render(src);
  EXPECT_NE(r.find("2 | | filter x"), std::string::npos) << r;
  EXPECT_NE(r.find("|          ^"), std::string::npos) << r;
}
