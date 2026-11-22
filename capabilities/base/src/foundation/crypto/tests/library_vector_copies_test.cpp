// The libraries' frozen vector files, copied into pp-browser (docs/contracts/SIGNED_EXTENSIONS.md § Vector files):
// pp-cpp-common's ext_area_vectors.json and pp-cpp-crypto's alg_id_vectors.json are the same bytes as the pinned tags'
// (the relay copies them from here). After a library bump, PP_WRITE_EXT_AREA_VECTORS=<path> /
// PP_WRITE_ALG_ID_VECTORS=<path> write the pinned tag's file there (normally the committed copy's path).

#include "common/tests/VectorFile.h"

#include <gtest/gtest.h>

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
  EXPECT_EQ(kExtArea.ReadText(), Upstream(PP_EXT_AREA_VECTORS_UPSTREAM)) << kExtArea.RegenerateHint();
}

TEST(LibraryVectorCopiesTest, TheAlgIdCopyIsPpCppCryptosFile) {
  EXPECT_EQ(kAlgId.ReadText(), Upstream(PP_ALG_ID_VECTORS_UPSTREAM)) << kAlgId.RegenerateHint();
}

} // namespace
} // namespace pbr
