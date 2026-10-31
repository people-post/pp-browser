// System-control families: what a feature binds, and how messaging finds the family of a message.
#include "common/chat/SystemControl.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

ThreadMessage Control(const std::string& control_type) {
  ThreadMessage message;
  message.content_type = ChatContentType::System;
  message.payload_json = std::string(R"({"control_type":")") + control_type + R"("})";
  return message;
}

SystemControlFamily Family(const std::string& name, const std::string& control_type) {
  SystemControlFamily family;
  family.name = name;
  family.owns = [control_type](const ThreadMessage& message) {
    return message.payload_json.find("\"" + control_type + "\"") != std::string::npos;
  };
  return family;
}

TEST(SystemControlRegistryTest, FindsTheFamilyThatOwnsASystemMessage) {
  SystemControlRegistry registry;
  EXPECT_FALSE(registry.Find(Control("call_invite")));
  registry.Bind(Family("call", "call_invite"));
  ASSERT_TRUE(registry.Find(Control("call_invite")));
  EXPECT_EQ(registry.Find(Control("call_invite"))->name, "call");
  EXPECT_FALSE(registry.Find(Control("group_invite"))) << "not claimed: a plain system message";

  ThreadMessage text = Control("call_invite");
  text.content_type = ChatContentType::Text;
  EXPECT_FALSE(registry.Find(text)) << "only system messages are control";
}

TEST(SystemControlRegistryTest, BindReplacesAndUnbindRemoves) {
  SystemControlRegistry registry;
  registry.Bind(Family("call", "call_invite"));
  auto replacement = Family("call", "call_invite");
  replacement.on_lane = [](const ThreadMessage&) { return true; };
  registry.Bind(replacement);
  EXPECT_EQ(registry.Snapshot()->size(), 1u) << "one family per name";
  EXPECT_TRUE(registry.OnLane(Control("call_invite")));

  const auto before = registry.Snapshot();
  registry.Unbind("call");
  EXPECT_FALSE(registry.Find(Control("call_invite")));
  EXPECT_FALSE(registry.OnLane(Control("call_invite"))) << "unbound: nothing rides the lane";
  EXPECT_EQ(before->size(), 1u) << "a snapshot taken before stays whole";
}

TEST(SystemControlRegistryTest, LaneMembershipIsPerMessage) {
  SystemControlRegistry registry;
  auto family = Family("call", "call_");
  family.owns = [](const ThreadMessage& message) { return message.payload_json.find("call_") != std::string::npos; };
  family.on_lane = [](const ThreadMessage& message) {
    return message.payload_json.find("call_summary") == std::string::npos;
  };
  registry.Bind(family);
  EXPECT_TRUE(registry.OnLane(Control("call_invite")));
  EXPECT_FALSE(registry.OnLane(Control("call_summary"))) << "history stays in the chat stream";
  EXPECT_FALSE(registry.OnLane(Control("charge_required"))) << "not the family's";
}

TEST(SystemControlRegistryTest, LiveAndQuietComeFromTheFamilies) {
  SystemControlRegistry registry;
  bool live = false;
  auto family = Family("call", "call_invite");
  family.live = [&live]() { return live; };
  registry.Bind(family);

  EXPECT_FALSE(registry.LiveFamily());
  live = true;
  EXPECT_EQ(registry.LiveFamily(), std::optional<std::string>("call"));
  EXPECT_FALSE(registry.Quiet(Control("call_invite"))) << "no quiet rule: not quiet";
}

} // namespace
} // namespace pbr
