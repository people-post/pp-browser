// The iPhone notifications name assets/sounds/call.caf and msg.caf (IosLocalNotifier, relay aps.sound).
// UNNotificationSound needs them to exist, be at most 30 s, and stay small.
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

TEST(IosNotificationSoundAssetTest, FilesExistAndAreUnderBudget) {
  for (const char* name : {"call.caf", "msg.caf"}) {
    const std::filesystem::path p = std::filesystem::path(PP_BROWSER_ASSETS_DIR) / "sounds" / name;
    ASSERT_TRUE(std::filesystem::exists(p)) << p;
    EXPECT_LT(std::filesystem::file_size(p), 1536u * 1024u) << name;
    std::ifstream in(p, std::ios::binary);
    char magic[4] = {};
    in.read(magic, 4);
    EXPECT_EQ(std::string(magic, 4), "caff") << name;
  }
}

TEST(IosNotificationSoundAssetTest, IosBundleCopiesThemToResourcesRoot) {
  std::ifstream in(std::string(PP_SOURCE_DIR) + "/cmake/IosBundle.cmake");
  std::string all((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  EXPECT_NE(all.find("foreach(_ios_snd call.caf msg.caf)"), std::string::npos);
}
