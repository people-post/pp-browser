// Call control as a system-control family: what is hidden, and how history lines read.
#include "feature/calls/CallControlFamily.h"

#include "domain/calls/CallControlCodec.h"
#include "foundation/i18n/LocalizationService.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

class CallControlFamilyTest : public ::testing::Test {
protected:
  void SetUp() override {
#ifdef PP_BROWSER_ASSETS_DIR
    ASSERT_TRUE(LocalizationService::Instance().LoadFromAssets(PP_BROWSER_ASSETS_DIR));
#endif
    LocalizationService::Instance().SetPreferredLanguage("en");
    labels_.peer = [](const std::string& identity) { return identity == "account:bob" ? "Bob" : ""; };
  }

  static ThreadMessage Message(CallControlType type, const std::string& sender, const std::string& detail = "{}") {
    return CallControlCodec::BuildSystemMessage("thread:t", type, "text", detail, sender).value();
  }

  SystemControlLabels labels_;
};

TEST_F(CallControlFamilyTest, PlumbingAndRosterUpdatesAreHidden) {
  const SystemControlFamily family = MakeCallControlFamily({});
  EXPECT_TRUE(family.hidden(Message(CallControlType::CallMediaKey, "account:bob")));
  EXPECT_TRUE(family.hidden(Message(CallControlType::CallIce, "account:bob")));
  EXPECT_TRUE(family.hidden(Message(CallControlType::CallRoster, "account:bob")))
      << "every mute or camera change sends one: no line per update";
  EXPECT_FALSE(family.hidden(Message(CallControlType::CallAccept, "account:bob")));
  EXPECT_TRUE(family.apply_out_of_order) << "media keys and accepts are idempotent";
  EXPECT_FALSE(family.line(Message(CallControlType::CallRoster, "account:bob"), labels_));
}

TEST_F(CallControlFamilyTest, HistoryLinesNameWhoDidWhat) {
  const SystemControlFamily family = MakeCallControlFamily({});
  EXPECT_EQ(family.line(Message(CallControlType::CallAccept, "account:bob"), labels_),
            std::optional<std::string>("Bob joined the call"));
  EXPECT_EQ(family.line(Message(CallControlType::CallAccept, kLocalSelfContactId), labels_),
            std::optional<std::string>("You joined the call"));
  const auto unknown = family.line(Message(CallControlType::CallLeave, "account:carol"), labels_);
  ASSERT_TRUE(unknown);
  EXPECT_EQ(unknown->find("account:carol"), std::string::npos) << "an unknown peer is not shown by id";
}

TEST_F(CallControlFamilyTest, BindsTheCallStacksState) {
  bool live = true;
  int received = 0;
  CallControlFamilyPorts ports;
  ports.receive = [&](const ThreadMessage&, const SystemControlContext& context) {
    EXPECT_EQ(context.sender_identity, "account:bob");
    ++received;
  };
  ports.has_active_local_call = [&]() { return live; };
  ports.active_call_origin_thread_id = []() { return std::optional<std::string>("thread:origin"); };
  SystemControlFamily family = MakeCallControlFamily(ports);
  ThreadMessage invite = Message(CallControlType::CallInvite, "account:bob");
  EXPECT_TRUE(family.owns(invite));
  ASSERT_TRUE(family.apply(invite, SystemControlContext{"account:bob", std::nullopt, std::nullopt}));
  EXPECT_EQ(received, 1);
  EXPECT_TRUE(family.live());
  EXPECT_EQ(family.preferred_public_thread(), std::optional<std::string>("thread:origin"));
}

} // namespace
} // namespace pbr
