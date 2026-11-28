#include "app/node/NodeServiceAsks.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <string>

namespace {

#if defined(_WIN32)
void SetEnv(const char* name, const char* value) {
  _putenv_s(name, value);
}
void UnsetEnv(const char* name) {
  _putenv_s(name, "");
}
#else
void SetEnv(const char* name, const char* value) {
  setenv(name, value, 1);
}
void UnsetEnv(const char* name) {
  unsetenv(name);
}
#endif

struct EnvGuard {
  std::string name;
  explicit EnvGuard(const char* n) : name(n) { UnsetEnv(name.c_str()); }
  ~EnvGuard() { UnsetEnv(name.c_str()); }
};

pbr::AppConfig WithRelay(const bool enabled) {
  pbr::AppConfig config;
  pbr::Object relay;
  relay.set("enabled", enabled);
  relay.set("retention_days", int64_t{3});
  config.node.set("relay", relay);
  return config;
}

const std::vector<std::string> kKnown = {"ledger", "relay"};

} // namespace

// projects/node-relay NR001, rule 4: asking for a service the build lacks fails the start.
TEST(NodeServiceAsksTest, AnAskForAServiceThisBuildLacksFails) {
  const auto asks = pbr::CheckPpNodeServiceAsks(WithRelay(true), {"ledger"}, kKnown);
  ASSERT_FALSE(static_cast<bool>(asks));
  EXPECT_NE(asks.error().message.find("node.relay.enabled is on, but this build has no relay"), std::string::npos);
}

TEST(NodeServiceAsksTest, ASectionWithoutAnAskIsOnlyIgnored) {
  const auto asks = pbr::CheckPpNodeServiceAsks(WithRelay(false), {"ledger"}, kKnown);
  ASSERT_TRUE(static_cast<bool>(asks));
  EXPECT_EQ(*asks, std::vector<std::string>{"relay"});
}

TEST(NodeServiceAsksTest, ABuildWithTheServiceHasNothingToCheck) {
  const auto asks = pbr::CheckPpNodeServiceAsks(WithRelay(true), {"ledger", "relay"}, kKnown);
  ASSERT_TRUE(static_cast<bool>(asks));
  EXPECT_TRUE(asks->empty());
}

TEST(NodeServiceAsksTest, TheEnvironmentAsksToo) {
  EnvGuard guard("PP_NODE_RELAY_ENABLED");
  pbr::AppConfig config = WithRelay(false);
  SetEnv("PP_NODE_RELAY_ENABLED", "yes");
  pbr::ApplyPpNodeServiceEnvOverlays(config, kKnown);
  const pbr::Object* relay = config.node.getObject("relay");
  ASSERT_NE(relay, nullptr);
  EXPECT_TRUE(relay->getIf<bool>("enabled").value_or(false));
  EXPECT_EQ(relay->getIf<int64_t>("retention_days").value_or(0), 3) << "the rest of the section stays";
  EXPECT_FALSE(static_cast<bool>(pbr::CheckPpNodeServiceAsks(config, {"ledger"}, kKnown)));

  SetEnv("PP_NODE_RELAY_ENABLED", "maybe");
  pbr::AppConfig untouched = WithRelay(false);
  pbr::ApplyPpNodeServiceEnvOverlays(untouched, kKnown);
  EXPECT_FALSE(untouched.node.getObject("relay")->getIf<bool>("enabled").value_or(true)) << "unparsable: left";
}

TEST(NodeServiceAsksTest, TheLedgerIsAskedOnlyByItsRole) {
  EnvGuard guard("PP_NODE_LEDGER_ENABLED");
  SetEnv("PP_NODE_LEDGER_ENABLED", "1");
  pbr::AppConfig config;
  pbr::ApplyPpNodeServiceEnvOverlays(config, kKnown);
  EXPECT_EQ(config.node.getObject("ledger"), nullptr);
  pbr::Object ledger;
  ledger.set("enabled", true);
  config.node.set("ledger", ledger);
  EXPECT_TRUE(static_cast<bool>(pbr::CheckPpNodeServiceAsks(config, {}, kKnown))) << "--ledger-role asks for it";
}
