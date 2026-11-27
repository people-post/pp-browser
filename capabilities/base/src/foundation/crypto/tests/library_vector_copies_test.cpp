// The libraries' frozen vector files, as pp-specs holds them (docs/contracts/SIGNED_EXTENSIONS.md § Vector files):
// pp-specs' ext_area_vectors.json and alg_id_vectors.json are the same bytes as pp-cpp-common's and pp-cpp-crypto's
// at their pinned tags, while the libraries still keep their own. After a library bump, PP_WRITE_EXT_AREA_VECTORS=<path>
// / PP_WRITE_ALG_ID_VECTORS=<path> write the pinned tag's file there, for a pp-specs pull request.

#include "common/tests/VectorFile.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <utility>
#include <fstream>
#include <iterator>
#include <string>

namespace pbr {
namespace {

using test::VectorFile;

const VectorFile kExtArea{PP_EXT_AREA_VECTORS_PATH, "PP_WRITE_EXT_AREA_VECTORS"};
const VectorFile kAlgId{PP_ALG_ID_VECTORS_PATH, "PP_WRITE_ALG_ID_VECTORS"};

std::string Upstream(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  EXPECT_TRUE(file.good()) << "the pinned library's file is not at " << path;
  return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

// The vectors are JSON, so line endings are not content: a Windows checkout of pp-specs (which sets no .gitattributes)
// may turn LF into CRLF, while the libraries' own files are marked -text.
std::string WithoutCr(std::string text) {
  text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
  return text;
}

TEST(LibraryVectorCopiesTest, LineEndingsAreNotCompared) {
  EXPECT_EQ(WithoutCr("a\r\nb\r\n"), WithoutCr("a\nb\n"));
}

TEST(LibraryVectorCopiesTest, WriteFreshCopiesWhenAsked) {
  bool wrote = false;
  for (const auto& [file, upstream] :
       {std::pair{&kExtArea, PP_EXT_AREA_VECTORS_UPSTREAM}, std::pair{&kAlgId, PP_ALG_ID_VECTORS_UPSTREAM}}) {
    if (file->WriteRequested()) {
      ASSERT_TRUE(file->WriteText(Upstream(upstream)));
      wrote = true;
    }
  }
  if (!wrote) {
    GTEST_SKIP() << kExtArea.SkipReason() << "; " << kAlgId.SkipReason();
  }
}

// RC020-VEC: the copies are the pinned tags' bytes.
TEST(LibraryVectorCopiesTest, TheExtAreaCopyIsPpCppCommonsFile) {
  EXPECT_EQ(WithoutCr(kExtArea.ReadText()), WithoutCr(Upstream(PP_EXT_AREA_VECTORS_UPSTREAM)))
      << kExtArea.RegenerateHint();
}

TEST(LibraryVectorCopiesTest, TheAlgIdCopyIsPpCppCryptosFile) {
  EXPECT_EQ(WithoutCr(kAlgId.ReadText()), WithoutCr(Upstream(PP_ALG_ID_VECTORS_UPSTREAM))) << kAlgId.RegenerateHint();
}

} // namespace
} // namespace pbr
