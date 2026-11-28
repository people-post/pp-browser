// The libraries' frozen vector files, as pp-specs holds them (docs/contracts/SIGNED_EXTENSIONS.md § Vector files):
// pp-specs' alg_id_vectors.json is the same bytes as pp-cpp-crypto's at its pinned tag, while that library still keeps
// its own. After a bump, PP_WRITE_ALG_ID_VECTORS=<path> writes the pinned tag's file there, for a pp-specs pull request.
// pp-cpp-common keeps no copy since v1.5.2 (it reads ext_area from pp-specs too), so its half is gone; this file goes
// when pp-cpp-crypto's pin reaches a release without its copy.

#include "common/tests/VectorFile.h"

#include <gtest/gtest.h>

#include <fstream>
#include <iterator>
#include <string>

namespace pbr {
namespace {

using test::VectorFile;

const VectorFile kAlgId{PP_ALG_ID_VECTORS_PATH, "PP_WRITE_ALG_ID_VECTORS"};

std::string Upstream(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  EXPECT_TRUE(file.good()) << "the pinned library's file is not at " << path;
  return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

TEST(LibraryVectorCopiesTest, WriteFreshCopiesWhenAsked) {
  if (!kAlgId.WriteRequested()) {
    GTEST_SKIP() << kAlgId.SkipReason();
  }
  ASSERT_TRUE(kAlgId.WriteText(Upstream(PP_ALG_ID_VECTORS_UPSTREAM)));
}

// RC020-VEC: the copy is the pinned tag's bytes.

TEST(LibraryVectorCopiesTest, TheAlgIdCopyIsPpCppCryptosFile) {
  EXPECT_EQ(kAlgId.ReadText(), Upstream(PP_ALG_ID_VECTORS_UPSTREAM)) << kAlgId.RegenerateHint();
}

} // namespace
} // namespace pbr
