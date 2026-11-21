#pragma once

#include "common/Value.h"
#include "common/ValueJson.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace pbr::test {

/**
 * A frozen JSON vector file (docs/contracts/SIGNED_EXTENSIONS.md § Vector files; release-compatibility SIGNED_FORMATS
 * § 6): committed beside its test, read by every side that builds the bytes, written only on request.
 *
 * The committed path comes from the test target's definition (`PP_<NAME>_VECTORS_PATH`, the source tree's file); the
 * writer's from an environment variable (`PP_WRITE_<NAME>_VECTORS=<path>`, usually that same path). The pattern:
 *
 *   const VectorFile kFile{PP_FOO_VECTORS_PATH, "PP_WRITE_FOO_VECTORS"};
 *
 *   TEST(FooVectorsTest, WriteAFreshFileWhenAsked) {
 *     if (!kFile.WriteRequested()) GTEST_SKIP() << kFile.SkipReason();
 *     ASSERT_TRUE(kFile.Write(BuildVectors()));
 *   }
 *   TEST(FooVectorsTest, TheCommittedFileIsWhatTheBuildersMake) {
 *     EXPECT_EQ(VectorFile::Text(WithoutKeys(kFile.Read(), kSignatureKeys)),
 *               VectorFile::Text(WithoutKeys(BuildVectors(), kSignatureKeys))) << kFile.RegenerateHint();
 *   }
 *
 * ML-DSA signing is hedged, so signatures differ between runs: compare `WithoutKeys(…, {"signature", …})` and check
 * the committed signatures verify instead.
 */
class VectorFile {
public:
  VectorFile(std::string committed_path, std::string write_env)
      : committed_path_(std::move(committed_path)), write_env_(std::move(write_env)) {}

  const std::string& committed_path() const { return committed_path_; }
  const std::string& write_env() const { return write_env_; }

  /** The committed file's bytes, exactly (empty with a test failure when it cannot be read). */
  std::string ReadText() const {
    std::ifstream file(committed_path_, std::ios::binary);
    EXPECT_TRUE(file.good()) << "cannot read " << committed_path_;
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
  }

  /** The committed file as a JSON object (empty with a test failure when it is not one). */
  pp::common::Object Read() const {
    const std::string text = ReadText();
    std::optional<pp::common::Object> object = pp::common::TryParseObject(text);
    EXPECT_TRUE(object.has_value()) << committed_path_ << " is not a JSON object";
    return object.value_or(pp::common::Object{});
  }

  /** Where to write, when the environment asks for it. */
  std::optional<std::string> WritePath() const {
    const char* out = std::getenv(write_env_.c_str());
    if (out == nullptr || *out == '\0') {
      return std::nullopt;
    }
    return std::string(out);
  }
  bool WriteRequested() const { return WritePath().has_value(); }

  /** Write `vectors` as the file's text (`Text`) to `WritePath()`. */
  ::testing::AssertionResult Write(const pp::common::Object& vectors) const { return WriteText(Text(vectors)); }

  /** Write `text` as it is (a copy of another repo's file) to `WritePath()`. */
  ::testing::AssertionResult WriteText(const std::string& text) const {
    const std::optional<std::string> out = WritePath();
    if (!out) {
      return ::testing::AssertionFailure() << write_env_ << " is not set";
    }
    std::ofstream file(*out, std::ios::binary | std::ios::trunc);
    file << text;
    file.close();
    if (!file.good()) {
      return ::testing::AssertionFailure() << "cannot write " << *out;
    }
    return ::testing::AssertionSuccess();
  }

  std::string SkipReason() const { return "set " + write_env_ + "=<path> to write " + FileName(); }
  std::string RegenerateHint() const { return "regenerate with " + write_env_ + "=" + committed_path_; }

  /** How every vector file is written: JSON with a two-space indent and a final newline. */
  static std::string Text(const pp::common::Object& vectors) { return pp::common::DumpJson(vectors, 2) + "\n"; }

private:
  std::string FileName() const { return std::filesystem::path(committed_path_).filename().string(); }

  std::string committed_path_;
  std::string write_env_;
};

/** `value` with every object member named in `keys` left out, at any depth (the randomized signatures). */
inline pp::common::Value WithoutKeys(const pp::common::Value& value, const std::set<std::string>& keys) {
  if (const pp::common::Object* object = pp::common::asObject(value)) {
    pp::common::Object out;
    for (const auto& [key, item] : object->entries()) {
      if (!keys.contains(key)) {
        out.set(key, WithoutKeys(item, keys));
      }
    }
    return pp::common::ObjectValue(std::move(out));
  }
  if (const pp::common::Array* array = pp::common::asArray(value)) {
    std::vector<pp::common::Value> out;
    for (const pp::common::Value& item : array->elements) {
      out.push_back(WithoutKeys(item, keys));
    }
    return pp::common::ArrayValue(std::move(out));
  }
  return value;
}

inline pp::common::Object WithoutKeys(const pp::common::Object& object, const std::set<std::string>& keys) {
  const pp::common::Value stripped = WithoutKeys(pp::common::ObjectValue(object), keys);
  const pp::common::Object* out = pp::common::asObject(stripped);
  return out != nullptr ? *out : pp::common::Object{};
}

} // namespace pbr::test
