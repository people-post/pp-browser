#include "lib/messaging/AiImageRef.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

TEST(AiImageRefTest, ImageRefRoundTripsThroughJson) {
  AiImageRef ref;
  for (int i = 0; i < 32; ++i) {
    ref.content_hash.push_back(static_cast<uint8_t>(i * 7 + 1));
  }
  ref.mime = "image/png";
  ref.width = 1200;
  ref.height = 800;
  const std::string json = AiImageRefToJson(ref);
  EXPECT_NE(json.find("\"content_hash\":\"01080f"), std::string::npos);
  const auto back = AiImageRefFromJson(json);
  ASSERT_TRUE(back.has_value());
  EXPECT_EQ(back->content_hash, ref.content_hash);
  EXPECT_EQ(back->mime, "image/png");
  EXPECT_EQ(back->width, 1200);
  EXPECT_EQ(back->height, 800);
}

TEST(AiImageRefTest, ImageRefRejectsBadJson) {
  EXPECT_FALSE(AiImageRefFromJson("").has_value());
  EXPECT_FALSE(AiImageRefFromJson("not json").has_value());
  EXPECT_FALSE(AiImageRefFromJson(R"({"content_hash":"abcd","mime":"image/png"})").has_value()); // short hash
  EXPECT_FALSE(AiImageRefFromJson(R"({"content_hash":")" + std::string(64, 'g') + R"(","mime":"image/png"})").has_value());
  EXPECT_FALSE(AiImageRefFromJson(R"({"content_hash":")" + std::string(64, 'a') + R"("})").has_value()); // no mime
  const auto no_size = AiImageRefFromJson(R"({"content_hash":")" + std::string(64, 'a') + R"(","mime":"image/jpeg"})");
  ASSERT_TRUE(no_size.has_value());
  EXPECT_EQ(no_size->width, 0);
  EXPECT_EQ(no_size->height, 0);
}

} // namespace
} // namespace pbr
