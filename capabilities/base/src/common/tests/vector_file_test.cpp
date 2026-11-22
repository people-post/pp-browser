// The vector-file harness (common/tests/VectorFile.h): reading the committed file, writing only on request, the text
// every vector file is written in, and leaving the randomized signatures out of a comparison.

#include "common/tests/VectorFile.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace pbr::test {
namespace {

using pp::common::Object;

void SetEnv(const char* name, const std::string& value) {
#ifdef _WIN32
  _putenv_s(name, value.c_str());
#else
  if (value.empty()) {
    unsetenv(name);
  } else {
    setenv(name, value.c_str(), 1);
  }
#endif
}

class VectorFileTest : public ::testing::Test {
protected:
  void SetUp() override {
    dir_ = std::filesystem::temp_directory_path() /
           ("pp_vector_file_test_" + std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()));
    std::filesystem::create_directories(dir_);
    SetEnv(kEnv, "");
  }
  void TearDown() override {
    SetEnv(kEnv, "");
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
  }
  std::string PathOf(const char* name) const { return (dir_ / name).string(); }

  static constexpr char kEnv[] = "PP_WRITE_VECTOR_FILE_TEST_VECTORS";
  std::filesystem::path dir_;
};

Object Sample() {
  Object inner;
  inner.set("signature", std::string("random"));
  inner.set("bytes_hex", std::string("00ff"));
  Object root;
  root.set("format", std::string("sample v1"));
  root.set("row", inner);
  root.set("rows", pp::common::ArrayValue({pp::common::ObjectValue(inner)}));
  return root;
}

TEST_F(VectorFileTest, NothingIsWrittenUnlessAsked) {
  const VectorFile file{PathOf("v.json"), kEnv};
  EXPECT_FALSE(file.WriteRequested());
  EXPECT_FALSE(file.Write(Sample()));
  EXPECT_FALSE(std::filesystem::exists(PathOf("v.json")));
  EXPECT_NE(file.SkipReason().find(kEnv), std::string::npos);
  EXPECT_NE(file.SkipReason().find("v.json"), std::string::npos);
}

TEST_F(VectorFileTest, AWrittenFileReadsBackAsItsText) {
  const VectorFile file{PathOf("v.json"), kEnv};
  SetEnv(kEnv, PathOf("v.json"));
  ASSERT_TRUE(file.WriteRequested());
  ASSERT_TRUE(file.Write(Sample()));
  EXPECT_EQ(file.ReadText(), VectorFile::Text(Sample()));
  EXPECT_EQ(file.Read(), Sample());
  EXPECT_EQ(VectorFile::Text(Sample()).back(), '\n');
  EXPECT_NE(VectorFile::Text(Sample()).find("\n  \"format\""), std::string::npos); // two-space indent
}

TEST_F(VectorFileTest, WriteTextWritesBytesAsTheyAre) {
  const VectorFile file{PathOf("copy.json"), kEnv};
  SetEnv(kEnv, PathOf("copy.json"));
  ASSERT_TRUE(file.WriteText("{\"a\":1}"));
  EXPECT_EQ(file.ReadText(), "{\"a\":1}");
  EXPECT_NE(file.RegenerateHint().find(kEnv), std::string::npos);
}

TEST(VectorFileWithoutKeysTest, NamedKeysAreLeftOutAtAnyDepth) {
  const Object stripped = WithoutKeys(Sample(), {"signature"});
  const std::string text = VectorFile::Text(stripped);
  EXPECT_EQ(text.find("signature"), std::string::npos) << text;
  EXPECT_NE(text.find("bytes_hex"), std::string::npos) << text;
  EXPECT_EQ(stripped.getString("format").value_or(""), "sample v1");
  EXPECT_EQ(WithoutKeys(Sample(), {}), Sample());
}

} // namespace
} // namespace pbr::test
