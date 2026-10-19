#include "foundation/data/Config.h"
#include "foundation/platform/IPathProvider.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <random>
#include <string>

using namespace pbr;

namespace {

/** The app's config directory in a temp dir, holding a config.json. */
class TempConfigDir : public IPathProvider {
public:
  explicit TempConfigDir(std::filesystem::path dir) : dir_(std::move(dir)) {}
  std::string ConfigDir() const override { return dir_.string(); }
  std::string DataDir(const std::string& override_path) const override { return override_path; }
  std::string CacheDir(const std::string& data_dir) const override { return data_dir; }
  std::string BundleAssetsDir() const override { return {}; }

private:
  std::filesystem::path dir_;
};

class ConfigDiscoverTest : public ::testing::Test {
protected:
  void SetUp() override {
    if (std::getenv("PP_BROWSER_CONFIG")) {
      GTEST_SKIP() << "PP_BROWSER_CONFIG is set in this environment";
    }
    dir_ = std::filesystem::temp_directory_path() / ("pp-config-discover-" + std::to_string(std::random_device{}()));
    std::filesystem::create_directories(dir_);
    std::ofstream(dir_ / "config.json") << "{}";
    provider_ = std::make_unique<TempConfigDir>(dir_);
    IPathProvider::SetInstance(provider_.get());
  }
  void TearDown() override {
    IPathProvider::SetInstance(nullptr);
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
  }

  std::filesystem::path dir_;
  std::unique_ptr<TempConfigDir> provider_;
};

} // namespace

TEST_F(ConfigDiscoverTest, TheAppFallsBackToItsOwnConfigFile) {
  char arg0[] = "pp-browser";
  char* argv[] = {arg0, nullptr};
  EXPECT_EQ(Config::DiscoverConfigPath(1, argv), (dir_ / "config.json").string());
}

// pp-node beside the desktop app of the same user must not take the app's settings (its mesh, its
// bootstrap peers, its model): only a file named for it.
TEST_F(ConfigDiscoverTest, WithoutAFallbackOnlyANamedFileIsUsed) {
  char arg0[] = "pp-node";
  char* argv[] = {arg0, nullptr};
  EXPECT_TRUE(Config::DiscoverConfigPath(1, argv, Config::Fallback::None).empty());

  char flag[] = "--config";
  char path[] = "/etc/pp-node/config.json";
  char* named[] = {arg0, flag, path, nullptr};
  EXPECT_EQ(Config::DiscoverConfigPath(3, named, Config::Fallback::None), "/etc/pp-node/config.json");
}
