#include "foundation/data/SessionStore.h"

#include "foundation/data/Config.h"
#include "foundation/data/ConfigJson.h"
#include "common/ValueJson.h"

#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <string>

namespace pbr {
namespace {

std::string ReadAll(const std::string& path) {
  std::ifstream in(path);
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

class FakeSecrets : public IConfigSecrets {
public:
  bool Ready() const override { return ready; }
  Roe<ConfigSecrets> Load() const override { return stored; }
  Roe<void> Store(const ConfigSecrets& secrets) override {
    stored = secrets;
    ++stores;
    return {};
  }
  bool ready = false;
  ConfigSecrets stored;
  int stores = 0;
};

class SessionStoreSecretsTest : public ::testing::Test {
protected:
  void SetUp() override {
    dir_ = std::filesystem::temp_directory_path() /
           ("pp_session_secrets_" + std::to_string(reinterpret_cast<uintptr_t>(this)));
    std::filesystem::remove_all(dir_);
    std::filesystem::create_directories(dir_);
    BootstrapResult bootstrap;
    bootstrap.config = Config::DefaultAppConfig();
    bootstrap.config_path = (dir_ / "config.json").string();
    bootstrap.profile_data_dir = dir_.string();
    store_.Initialize(std::move(bootstrap));
    store_.AddConfigListener([this](const AppConfig&) { ++notified_; });
    store_.SetConfigSecrets(&secrets_);
  }
  void TearDown() override { std::filesystem::remove_all(dir_); }

  std::string FileText() const { return ReadAll(store_.Snapshot().config_path); }

  std::filesystem::path dir_;
  SessionStore store_;
  FakeSecrets secrets_;
  int notified_ = 0;
};

// PB-034: with the vault open, a typed key goes to the secrets store and never into config.json, while the
// in-memory config (what the agent gets) still carries it.
TEST_F(SessionStoreSecretsTest, SaveKeepsTypedKeysOutOfTheFile) {
  secrets_.ready = true;
  AppConfig config = store_.Snapshot().config;
  config.llm.preset = "cloud";
  config.llm.api_key = "sk-typed";
  config.search.api_key = "tvly-typed";
  auto saved = store_.SaveConfig(config);
  ASSERT_TRUE(static_cast<bool>(saved)) << saved.error().message;

  EXPECT_EQ(secrets_.stored.llm_api_key, "sk-typed");
  EXPECT_EQ(secrets_.stored.search_api_key, "tvly-typed");
  const std::string file = FileText();
  EXPECT_EQ(file.find("sk-typed"), std::string::npos) << file;
  EXPECT_EQ(file.find("tvly-typed"), std::string::npos) << file;
  EXPECT_EQ(store_.Snapshot().config.llm.api_key, "sk-typed");
  EXPECT_EQ(store_.Snapshot().config.search.api_key, "tvly-typed");

  auto reloaded = store_.ReloadConfig();
  ASSERT_TRUE(static_cast<bool>(reloaded)) << reloaded.error().message;
  EXPECT_EQ(store_.Snapshot().config.llm.api_key, "sk-typed") << "a reload overlays the stored keys";
}

// A key referenced through api_key_env is not a typed key: it is neither stored nor stripped.
TEST_F(SessionStoreSecretsTest, EnvDerivedKeysAreLeftAlone) {
  secrets_.ready = true;
  AppConfig config = store_.Snapshot().config;
  config.llm.preset = "cloud";
  config.llm_api_key_env = "OPENAI_API_KEY";
  config.llm.api_key = "from-env";
  auto saved = store_.SaveConfig(config);
  ASSERT_TRUE(static_cast<bool>(saved)) << saved.error().message;
  EXPECT_TRUE(secrets_.stored.llm_api_key.empty());
  EXPECT_NE(FileText().find("OPENAI_API_KEY"), std::string::npos);
  EXPECT_EQ(FileText().find("from-env"), std::string::npos);
}

// Before the vault opens the file is written as before (nothing lost); once it opens, ApplyConfigSecrets
// moves the key from the file into the store and rewrites the file without it.
TEST_F(SessionStoreSecretsTest, ApplyMigratesAKeyFoundInTheFile) {
  secrets_.ready = false;
  AppConfig config = store_.Snapshot().config;
  config.llm.preset = "cloud";
  config.llm.api_key = "sk-legacy";
  ASSERT_TRUE(static_cast<bool>(store_.SaveConfig(config)));
  ASSERT_NE(FileText().find("sk-legacy"), std::string::npos) << "locked: still in the file";
  EXPECT_EQ(secrets_.stores, 0);

  secrets_.ready = true;
  auto applied = store_.ApplyConfigSecrets();
  ASSERT_TRUE(static_cast<bool>(applied)) << applied.error().message;
  EXPECT_EQ(secrets_.stored.llm_api_key, "sk-legacy");
  EXPECT_EQ(FileText().find("sk-legacy"), std::string::npos) << FileText();
  EXPECT_EQ(store_.Snapshot().config.llm.api_key, "sk-legacy");
}

// Nothing in the file, something in the store: Apply overlays and tells listeners once.
TEST_F(SessionStoreSecretsTest, ApplyOverlaysStoredKeysAndNotifies) {
  secrets_.ready = true;
  secrets_.stored.llm_api_key = "sk-stored";
  const int before = notified_;
  ASSERT_TRUE(static_cast<bool>(store_.ApplyConfigSecrets()));
  EXPECT_EQ(store_.Snapshot().config.llm.api_key, "sk-stored");
  EXPECT_EQ(notified_, before + 1);
  ASSERT_TRUE(static_cast<bool>(store_.ApplyConfigSecrets()));
  EXPECT_EQ(notified_, before + 1) << "no change, no second notification";
}

} // namespace
} // namespace pbr
