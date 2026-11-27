#include "domain/relay/RelaySettings.h"

#include <gtest/gtest.h>

#include <map>
#include <string>

#include "common/PbrCompat.h"

using namespace pbr;

namespace {

struct FakeEnv {
  std::map<std::string, std::string> vars;
  relay::RelaySettings::Env Lookup() const {
    return [this](const char* name) -> const char* {
      const auto it = vars.find(name);
      return it == vars.end() ? nullptr : it->second.c_str();
    };
  }
};

} // namespace

// projects/node-relay NR007: off until enabled (Hongwei, 2026-11-27), with the defaults below.
TEST(RelaySettingsTest, OffWithTheDefaultsWhenNothingIsWritten) {
  auto s = relay::RelaySettings::Read(nullptr, {});
  ASSERT_TRUE(static_cast<bool>(s)) << s.error().message;
  EXPECT_FALSE(s->enabled);
  EXPECT_EQ(s->retention_days, 7);
  EXPECT_EQ(s->max_records, 1000);
  EXPECT_EQ(s->max_bytes, 16 * 1024 * 1024);
  EXPECT_EQ(s->Describe(), "off (node.relay.enabled)");
}

TEST(RelaySettingsTest, TheFileSetsThemAndTheEnvironmentWins) {
  Object section;
  section.set("enabled", true);
  section.set("retention_days", int64_t{14});
  section.set("max_records", int64_t{500});
  FakeEnv env;
  env.vars["PP_NODE_RELAY_MAX_RECORDS"] = " 200 ";
  env.vars["PP_NODE_RELAY_MAX_BYTES"] = "1048576";
  auto s = relay::RelaySettings::Read(&section, env.Lookup());
  ASSERT_TRUE(static_cast<bool>(s)) << s.error().message;
  EXPECT_TRUE(s->enabled);
  EXPECT_EQ(s->retention_days, 14);
  EXPECT_EQ(s->max_records, 200);
  EXPECT_EQ(s->max_bytes, 1048576);
  EXPECT_EQ(s->Describe(), "on, keeps 14 days, 200 records / 1 MiB per account");

  env.vars["PP_NODE_RELAY_ENABLED"] = "off";
  EXPECT_FALSE(relay::RelaySettings::Read(&section, env.Lookup())->enabled);
}

TEST(RelaySettingsTest, AWrongValueIsAnErrorNamingItsKey) {
  Object wrong_type;
  wrong_type.set("retention_days", "seven");
  auto s = relay::RelaySettings::Read(&wrong_type, {});
  ASSERT_FALSE(static_cast<bool>(s));
  EXPECT_NE(s.error().message.find("node.relay.retention_days"), std::string::npos);

  Object too_long;
  too_long.set("retention_days", int64_t{90});
  s = relay::RelaySettings::Read(&too_long, {});
  ASSERT_FALSE(static_cast<bool>(s));
  EXPECT_NE(s.error().message.find("outside (1 to 30)"), std::string::npos);

  FakeEnv env;
  env.vars["PP_NODE_RELAY_MAX_BYTES"] = "lots";
  s = relay::RelaySettings::Read(nullptr, env.Lookup());
  ASSERT_FALSE(static_cast<bool>(s));
  EXPECT_NE(s.error().message.find("PP_NODE_RELAY_MAX_BYTES"), std::string::npos);

  env.vars["PP_NODE_RELAY_MAX_BYTES"] = "1024";
  EXPECT_FALSE(static_cast<bool>(relay::RelaySettings::Read(nullptr, env.Lookup()))) << "below one large record";

  Object not_bool;
  not_bool.set("enabled", "yes");
  EXPECT_FALSE(static_cast<bool>(relay::RelaySettings::Read(&not_bool, {})));
}

TEST(RelaySettingsTest, KeysItDoesNotKnowAreNamed) {
  Object section;
  section.set("retention_day", int64_t{30});
  std::vector<std::string> unknown;
  auto s = relay::RelaySettings::Read(&section, {}, &unknown);
  ASSERT_TRUE(static_cast<bool>(s));
  EXPECT_EQ(s->retention_days, 7) << "the typo did not set it";
  EXPECT_EQ(unknown, std::vector<std::string>{"retention_day"});
}
