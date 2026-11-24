// A custom backend reaches only its own addresses (profile-model P2, #741–#744). The config file is loaded the way the app
// loads it, the hub's settings and the backend's clients are made from it, and every request they try goes through the
// HTTP test gate, which records the URL and refuses it (no network). The build's prod, staging and dev are all compiled
// here: a request to any of them would be a fallback to another backend.
#include "feature/ai/ArticleFeedFetch.h"
#include "feature/product/HubSettingsProjection.h"
#include "foundation/data/Config.h"
#include "foundation/data/LlmPreset.h"
#include "foundation/data/PlatformDefaults.h"
#include "foundation/platform/Platform.h"
#include "foundation/data/UserPreferences.h"
#include "foundation/diagnostics/CrashReportEnvelope.h"
#include "foundation/platform/BackendTable.h"
#include "foundation/platform/DeploymentProfile.h"
#include "lib/delivery/BackendClientSet.h"
#include "lib/net/OrgBackendClientsImpl.h"
#include "lib/net/HttpClient.h"
#include "lib/net/SwitchableDirectoryClient.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

namespace {

constexpr pbr::CompiledOrigins kCompiled{.prod = "https://prod.example.test",
                                         .staging = "https://staging.example.test",
                                         .dev = "https://dev.example.test",
                                         .prod_relay_id = "relay:prod",
                                         .staging_relay_id = "relay:staging",
                                         .dev_relay_id = "relay:dev"};

class ScopedUnset {
public:
  explicit ScopedUnset(const char* name) : name_(name) {
    if (const char* old = std::getenv(name)) {
      had_ = true;
      old_ = old;
    }
    Set(nullptr);
  }
  ~ScopedUnset() { Set(had_ ? old_.c_str() : nullptr); }

private:
  void Set(const char* value) const {
#ifdef _WIN32
    _putenv_s(name_.c_str(), value ? value : "");
#else
    if (value) {
      setenv(name_.c_str(), value, 1);
    } else {
      unsetenv(name_.c_str());
    }
#endif
  }
  std::string name_;
  bool had_ = false;
  std::string old_;
};

/** Records every request the net layer is asked to make and refuses it. */
class RequestRecorder {
public:
  RequestRecorder() {
    pbr::HttpClient::SetRequestGateForTest([this](const std::string& method, const std::string& url) {
      std::lock_guard lock(mutex_);
      urls_.push_back(method + " " + url);
      return false;
    });
  }
  ~RequestRecorder() { pbr::HttpClient::SetRequestGateForTest(nullptr); }
  std::vector<std::string> Urls() const {
    std::lock_guard lock(mutex_);
    return urls_;
  }
  size_t Asked(const std::string& part) const {
    size_t n = 0;
    for (const std::string& url : Urls()) {
      n += url.find(part) != std::string::npos ? 1 : 0;
    }
    return n;
  }

private:
  mutable std::mutex mutex_;
  std::vector<std::string> urls_;
};

std::string CustomFile(const std::string& without) {
  std::string services;
  const std::vector<std::pair<std::string, std::string>> rows = {
      {"llm_gateway", "https://custom.example.test/api/llm/v1"},
      {"llm_token", "https://custom.example.test/api/llm/v1"},
      {"promoted_mcp", "https://custom.example.test/mcp"},
      {"crash_intake", "https://custom.example.test/api/relay"},
      {"account_locker", "https://custom.example.test/api/relay"},
      {"client_compat", "https://custom.example.test/api/relay"}};
  for (const auto& [key, value] : rows) {
    if (key != without) {
      services += std::string(services.empty() ? "" : ",") + " \"" + key + "\": \"" + value + "\"";
    }
  }
  const std::string directory =
      without == "directory" ? "{}" : R"({ "base_url": "https://custom.example.test/api/relay" })";
  return "{ \"config_version\": 1, \"backend\": { \"env\": \"custom\", \"services\": {" + services +
         " } }, \"llm\": { \"preset\": \"brief\" },"
         " \"relay\": { \"base_url\": \"https://custom.example.test/api/relay\", \"relay_id\": \"relay:custom\" },"
         " \"registration\": { \"base_url\": \"https://custom.example.test/api/relay\", \"relay_id\": \"relay:custom\" "
         "},"
         " \"directory\": " +
         directory + " }";
}

/** What ConversationsHub::UpdateOrgBackendClients hands the clients, from the hub's settings. */
pbr::BackendClientSet::Endpoints EndpointsOf(const pbr::HubSettings& settings) {
  return {.relay = settings.relay,
          .directory = settings.directory,
          .registration = settings.registration,
          .account_locker = settings.account_locker_url,
          .client_compat = settings.client_compat_url,
          .llm_token = settings.llm_token_url,
          .extra_relays = settings.relay_extra_base_urls};
}

/**
 * The hub's backend defaults as Bootstrap::Run sets them: after Config::Load latched the backend (the hub itself is built
 * before it). An empty endpoint falls back to these, so for a custom backend they must hold nothing of a built-in's.
 */
void SetHubDefaultsAsBootstrapDoes(pbr::BackendClientSet& clients) {
  const pbr::AppConfig defaults = pbr::PlatformDefaults::For(pbr::Platform::Detect());
  clients.SetDefaults({defaults.relay, defaults.directory, defaults.registration});
}

const pbr::SignedRelayCommand kSigned{.audience = "QmRelay", .timestamp = 1, .signature = "sig"};

} // namespace

// Each service in turn removed from a custom table: what needs it sends nothing, and every other request is to the
// custom backend's own host.
TEST(BackendReachTest, ACustomBackendReachesOnlyItsOwnUrlsWhateverServiceIsMissing) {
  const ScopedUnset no_origin("PP_BROWSER_ORIGIN");
  const ScopedUnset no_relay_id("PP_BROWSER_RELAY_ID");
  const char* const removals[] = {
      "", "llm_gateway", "llm_token", "directory", "promoted_mcp", "crash_intake", "account_locker", "client_compat"};
  for (const char* removed : removals) {
    SCOPED_TRACE(std::string("without: ") + removed);
    const pbr::ScopedCompiledOriginsForTest compiled(kCompiled);
    const std::string path = (std::filesystem::temp_directory_path() / "pp_backend_reach.json").string();
    {
      std::ofstream out(path);
      out << CustomFile(removed);
    }
    auto loaded = pbr::Config::LoadFromFile(path, false);
    std::filesystem::remove(path);
    ASSERT_TRUE(static_cast<bool>(loaded)) << loaded.error().message;
    const std::string gone = removed;

    const pbr::HubSettings settings = pbr::HubSettingsFrom(*loaded, pbr::ProfilePreferences{});
    EXPECT_EQ(settings.account_locker_url.empty(), gone == "account_locker");
    EXPECT_EQ(settings.client_compat_url.empty(), gone == "client_compat");
    EXPECT_EQ(settings.llm_token_url.empty(), gone == "llm_token");

    const RequestRecorder recorder;
    pbr::SwitchableDirectoryClient directory;
    pbr::BackendClientSet clients(directory);
    SetHubDefaultsAsBootstrapDoes(clients);
    clients.Apply(EndpointsOf(settings));
    ASSERT_NE(clients.Registration(), nullptr);
    // Every service, asked once.
    if (auto compat = clients.ClientCompat()) {
      (void)compat->Fetch();
    }
    (void)clients.Registration()->RenewLlmToken("account:me", kSigned);
    (void)directory.SearchPeople("alice");
    (void)directory.LookupByAccount("account:alice");
    if (auto locker = clients.Locker()) {
      (void)locker->Get("00ff");
    }

    // The absent service: no client, or no request.
    EXPECT_EQ(clients.ClientCompat() == nullptr, gone == "client_compat");
    EXPECT_EQ(clients.Locker() == nullptr, gone == "account_locker");
    EXPECT_EQ(directory.Configured(), gone != "directory");
    EXPECT_EQ(recorder.Asked("/v1/client-compat"), gone == "client_compat" ? 0U : 1U);
    EXPECT_EQ(recorder.Asked("/token/renew"), gone == "llm_token" ? 0U : 1U);
    EXPECT_EQ(recorder.Asked("/v1/lockers/"), gone == "account_locker" ? 0U : 1U);
    const size_t directory_requests = recorder.Urls().size() - recorder.Asked("/v1/client-compat") -
                                      recorder.Asked("/token/renew") - recorder.Asked("/v1/lockers/");
    if (gone == "directory") {
      EXPECT_EQ(directory_requests, 0U) << "no directory: nothing is asked";
    } else {
      EXPECT_GE(directory_requests, 1U) << "the directory is asked at the custom backend's own address";
    }

    // The token renews under the token service's address, not under the relay's origin.
    if (gone != "llm_token") {
      EXPECT_EQ(recorder.Asked("POST https://custom.example.test/api/llm/v1/token/renew"), 1U);
    }
    // Zero requests outside the custom backend's own host.
    for (const std::string& url : recorder.Urls()) {
      EXPECT_NE(url.find("https://custom.example.test/"), std::string::npos) << url;
      EXPECT_EQ(url.find("prod.example.test"), std::string::npos) << url;
      EXPECT_EQ(url.find("staging.example.test"), std::string::npos) << url;
      EXPECT_EQ(url.find("dev.example.test"), std::string::npos) << url;
    }

    // The LLM, the promoted MCP and the crash intake are not HTTP clients of this set: their addresses are the table's.
    EXPECT_EQ(pbr::BriefLlmBaseUrl().empty(), gone == "llm_gateway");
    EXPECT_EQ(pbr::BriefLlmServiceBaseUrl(*loaded),
              gone == "llm_gateway" ? "" : "https://custom.example.test/api/llm/v1");
    EXPECT_EQ(pbr::ResolvePromotedMcp(*loaded, pbr::Config::DefaultAppConfig(false)).url.empty(),
              gone == "promoted_mcp");
    const std::string crash_url = pbr::ResolveCrashReportUploadUrl(pbr::ActiveBackendTable().crash_intake_url, "");
    EXPECT_EQ(crash_url.empty(), gone == "crash_intake");
    if (!crash_url.empty()) {
      EXPECT_EQ(crash_url, "https://custom.example.test/api/relay/v1/crash-reports");
    }
  }
}

// With no token service named, a renew sends nothing at all — not to the relay's origin, not to another backend.
TEST(BackendReachTest, ATokenRenewWithoutATokenServiceSendsNothing) {
  const RequestRecorder recorder;
  pbr::HttpRegistrationClient client("https://custom.example.test/api/relay");
  auto renewed = client.RenewLlmToken("account:me", kSigned);
  EXPECT_FALSE(renewed);
  EXPECT_TRUE(recorder.Urls().empty());
  client.SetLlmTokenUrl("https://tokens.example.test/api/llm/v1/");
  (void)client.RenewLlmToken("account:me", kSigned);
  ASSERT_EQ(recorder.Urls().size(), 1U);
  EXPECT_EQ(recorder.Urls().front(), "POST https://tokens.example.test/api/llm/v1/token/renew");
}

// A built-in backend renews where it always did: under the origin's LLM prefix.
TEST(BackendReachTest, ABuiltInBackendsTokenRenewIsUnderItsOriginsLlmPrefix) {
  const ScopedUnset no_origin("PP_BROWSER_ORIGIN");
  const pbr::ScopedCompiledOriginsForTest compiled(kCompiled);
  const std::string path = (std::filesystem::temp_directory_path() / "pp_backend_reach_prod.json").string();
  {
    std::ofstream out(path);
    out << R"({ "config_version": 1, "backend": { "env": "prod" } })";
  }
  auto loaded = pbr::Config::LoadFromFile(path, false);
  std::filesystem::remove(path);
  ASSERT_TRUE(static_cast<bool>(loaded));
  const pbr::HubSettings settings = pbr::HubSettingsFrom(*loaded, pbr::ProfilePreferences{});
  const RequestRecorder recorder;
  pbr::SwitchableDirectoryClient directory;
  pbr::BackendClientSet clients(directory);
  clients.Apply(EndpointsOf(settings));
  (void)clients.Registration()->RenewLlmToken("account:me", kSigned);
  ASSERT_EQ(recorder.Urls().size(), 1U);
  EXPECT_EQ(recorder.Urls().front(), "POST https://prod.example.test/api/llm/v1/token/renew");
}

// A custom backend that names no relay, registration or directory at all: the hub's defaults (taken after the latch)
// hold nothing, so no client is made and nothing is sent anywhere — not to the build's prod, which is compiled here.
TEST(BackendReachTest, ACustomBackendWithNoRelayRegistrationOrDirectoryFallsBackToNothing) {
  const ScopedUnset no_origin("PP_BROWSER_ORIGIN");
  const ScopedUnset no_relay_id("PP_BROWSER_RELAY_ID");
  const pbr::ScopedCompiledOriginsForTest compiled(kCompiled);
  const std::string path = (std::filesystem::temp_directory_path() / "pp_backend_reach_bare.json").string();
  {
    std::ofstream out(path);
    out << R"({ "config_version": 1, "backend": { "env": "custom" } })";
  }
  auto loaded = pbr::Config::LoadFromFile(path, false);
  std::filesystem::remove(path);
  ASSERT_TRUE(static_cast<bool>(loaded));
  const RequestRecorder recorder;
  pbr::SwitchableDirectoryClient directory;
  pbr::BackendClientSet clients(directory);
  SetHubDefaultsAsBootstrapDoes(clients);
  const pbr::AppConfig defaults = pbr::PlatformDefaults::For(pbr::Platform::Detect());
  EXPECT_TRUE(defaults.relay.base_url.empty() && defaults.directory.base_url.empty() &&
              defaults.registration.base_url.empty());
  clients.Apply(EndpointsOf(pbr::HubSettingsFrom(*loaded, pbr::ProfilePreferences{})));
  EXPECT_EQ(clients.Relay(), nullptr);
  EXPECT_EQ(clients.Registration(), nullptr);
  EXPECT_EQ(clients.ClientCompat(), nullptr);
  EXPECT_EQ(clients.Locker(), nullptr);
  EXPECT_FALSE(directory.Configured());
  (void)directory.SearchPeople("alice");
  EXPECT_TRUE(recorder.Urls().empty());
  // What the app did before the fix: defaults taken before the latch are the build's fallback (prod's), and would have
  // been filled in. The test holds the ordering: a built-in table's defaults are not what a custom backend is given.
  const pbr::AppConfig prod_defaults =
      pbr::PlatformDefaults::ForTable(pbr::BuiltInBackendTable(pbr::BackendEnv::Prod, kCompiled));
  EXPECT_EQ(prod_defaults.relay.base_url, "https://prod.example.test/api/relay");
  EXPECT_NE(prod_defaults.relay.base_url, defaults.relay.base_url);
}

// A tester build with no prod origin compiled reads a file a store prod build saved (its promoted_mcp is the old write-back,
// unmarked): the feed fetch goes to the tester's own seed and nothing is sent to the file's address.
TEST(BackendReachTest, ATesterBuildReadingAStoreProdFileNeverCallsTheFileMcpAddress) {
  const ScopedUnset no_origin("PP_BROWSER_ORIGIN");
  const pbr::ScopedCompiledOriginsForTest compiled(pbr::CompiledOrigins{
      .staging = "https://staging.example.test", .staging_relay_id = "relay:staging", .kind = pbr::BuildKind::Tester});
  const std::string path = (std::filesystem::temp_directory_path() / "pp_backend_reach_tester_mcp.json").string();
  {
    std::ofstream out(path);
    out << R"({ "config_version": 1, "backend": { "env": "prod" },
                "promoted_mcp": { "url": "https://store-prod.example.test/mcp" } })";
  }
  auto loaded = pbr::Config::LoadFromFile(path, false);
  std::filesystem::remove(path);
  ASSERT_TRUE(static_cast<bool>(loaded));
  const RequestRecorder recorder;
  (void)pbr::FetchArticleFeed(*loaded, pbr::Object{});
  EXPECT_GE(recorder.Urls().size(), 1U) << "the feed is asked at the tester's own address";
  for (const std::string& url : recorder.Urls()) {
    EXPECT_NE(url.find("https://staging.example.test/mcp"), std::string::npos) << url;
    EXPECT_EQ(url.find("store-prod"), std::string::npos) << url;
  }
}
