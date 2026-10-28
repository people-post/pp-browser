// Permission cards that end (B114): the stored card message is rewritten by id, a tap on a dead card is told
// plainly, and every tool that asks for permission has plain-words copy in both languages.
#include "gui/chat/ApprovalCard.h"

#include "common/Utilities.h"
#include "feature/conversations/ConversationsFacade.h"
#include "feature/conversations/ConversationsHub.h"
#include "feature/conversations/MessagingTools.h"
#include "feature/settings/SettingsTools.h"
#include "foundation/i18n/LocalizationService.h"
#include "lib/ai/ToolRegistry.h"
#include "lib/messaging/SqliteThreadStore.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace pbr {
namespace {

ByteVector TestDek() {
  ByteVector dek(32);
  for (size_t i = 0; i < dek.size(); ++i) {
    dek[i] = static_cast<uint8_t>(0x30 + i);
  }
  return dek;
}

PlannedToolCall DarkModeCall() {
  PlannedToolCall call;
  call.name = "set_appearance";
  call.arguments.set("appearance", std::string("dark"));
  return call;
}

ThreadMessage CardMessage(const std::string& id) {
  ThreadMessage card;
  card.id = id;
  card.thread_id = "ai-thread";
  card.sender_contact_id = "ai";
  card.text = BuildToolPermissionChoiceBlocks("apr-1", {DarkModeCall()}, true);
  card.content_rml = "<div class=\"bubble bubble-assistant\"><button>Allow once</button></div>";
  card.chat_actions.push_back({"Allow once", "Allow once", R"({"type":"tool_permission","approval_id":"apr-1"})"});
  card.timestamp = util::NowUnixMs();
  return card;
}

std::string Rml(const ThreadMessage& message) {
  return message.content_rml.value_or("");
}

class ApprovalCardTest : public ::testing::TestWithParam<const char*> {
protected:
  void SetUp() override {
    ASSERT_TRUE(LocalizationService::Instance().LoadFromAssets(PP_BROWSER_ASSETS_DIR));
    LocalizationService::Instance().SetPreferredLanguage(GetParam());
    data_dir_ = std::filesystem::temp_directory_path() / ("pp_approval_card_" + util::GenerateUuid());
    std::filesystem::remove_all(data_dir_);
    store_ = std::make_unique<SqliteThreadStore>(data_dir_.string());
    ASSERT_TRUE(store_->SetDek(TestDek()));
    Thread thread;
    thread.id = "ai-thread";
    thread.kind = ThreadKind::Direct;
    thread.title = "AI";
    thread.updated_at = 1;
    ASSERT_TRUE(store_->UpsertThread(thread));
  }
  void TearDown() override {
    store_.reset();
    std::filesystem::remove_all(data_dir_);
    LocalizationService::Instance().SetPreferredLanguage("en");
  }

  std::filesystem::path data_dir_;
  std::unique_ptr<SqliteThreadStore> store_;
};

INSTANTIATE_TEST_SUITE_P(Locales, ApprovalCardTest, ::testing::Values("en", "zh-Hans"));

} // namespace

TEST_P(ApprovalCardTest, EachEndingTakesTheButtonsAway) {
  struct Case {
    ApprovalOutcome outcome;
    std::string text;
  };
  const std::vector<Case> cases = {{ApprovalOutcome::Stale, Tr("chat.permission.stale")},
                                   {ApprovalOutcome::AllowOnce, Tr("chat.permission.done.allow_once")},
                                   {ApprovalOutcome::AllowAlways, Tr("chat.permission.done.allow_always")},
                                   {ApprovalOutcome::Deny, Tr("chat.permission.done.deny")}};
  for (const Case& c : cases) {
    const auto closed = ClosedApprovalCardMessage(CardMessage("card-1"), c.outcome);
    ASSERT_TRUE(closed.has_value());
    EXPECT_EQ(closed->id, "card-1");
    EXPECT_EQ(closed->text.find("tool_permission"), std::string::npos) << closed->text;
    EXPECT_EQ(Rml(*closed).find("button"), std::string::npos) << Rml(*closed);
    EXPECT_EQ(Rml(*closed).find("Allow once"), std::string::npos) << Rml(*closed);
    EXPECT_NE(Rml(*closed).find(c.text), std::string::npos) << Rml(*closed);
    EXPECT_TRUE(closed->chat_actions.empty());
  }
  // Allowed / denied keep the explanation above the result line; stale replaces the whole card.
  const auto allowed = ClosedApprovalCardMessage(CardMessage("card-1"), ApprovalOutcome::AllowOnce);
  EXPECT_NE(Rml(*allowed).find(Tr("chat.permission.intro")), std::string::npos);
  const auto stale = ClosedApprovalCardMessage(CardMessage("card-1"), ApprovalOutcome::Stale);
  EXPECT_EQ(Rml(*stale).find(Tr("chat.permission.intro")), std::string::npos);
}

TEST_P(ApprovalCardTest, TheCardIsFoundByIdWithFiftyMessagesAfterIt) {
  ASSERT_TRUE(store_->AppendMessage(CardMessage("card-1")));
  for (int i = 0; i < 50; ++i) {
    ThreadMessage later;
    later.id = "later-" + std::to_string(i);
    later.thread_id = "ai-thread";
    later.sender_contact_id = "ai";
    later.text = "message " + std::to_string(i);
    later.timestamp = util::NowUnixMs() + i + 1;
    ASSERT_TRUE(store_->AppendMessage(later));
  }

  EXPECT_TRUE(CloseApprovalCardInStore(*store_, "ai-thread", "card-1", ApprovalOutcome::Stale));
  auto card = store_->GetMessage("ai-thread", "card-1");
  ASSERT_TRUE(card && card->has_value());
  EXPECT_EQ((*card)->text, BuildToolPermissionStaleBlocks(""));
  EXPECT_EQ(Rml(**card).find("Allow once"), std::string::npos);

  auto last = store_->GetMessage("ai-thread", "later-49");
  ASSERT_TRUE(last && last->has_value());
  EXPECT_EQ((*last)->text, "message 49"); // only the card changed

  EXPECT_FALSE(CloseApprovalCardInStore(*store_, "ai-thread", "no-such-card", ApprovalOutcome::Stale));
  EXPECT_FALSE(CloseApprovalCardInStore(*store_, "ai-thread", "", ApprovalOutcome::Stale));
}

TEST_P(ApprovalCardTest, AnAnsweredCardKeepsItsExplanationInTheStore) {
  ASSERT_TRUE(store_->AppendMessage(CardMessage("card-1")));
  ASSERT_TRUE(CloseApprovalCardInStore(*store_, "ai-thread", "card-1", ApprovalOutcome::Deny));
  auto card = store_->GetMessage("ai-thread", "card-1");
  ASSERT_TRUE(card && card->has_value());
  EXPECT_NE((*card)->text.find(Tr("chat.permission.intro")), std::string::npos);
  EXPECT_NE((*card)->text.find(Tr("chat.permission.done.deny")), std::string::npos);
  EXPECT_EQ((*card)->text.find("tool_permission"), std::string::npos);
}

TEST(ApprovalTapReactionTest, OnlyUnexpectedRefusalsRewriteAndNothingShowsARawCode) {
  EXPECT_EQ(ReactionToResumeError(ApprovalResumeErrorCode(ApprovalResumeError::AlreadyResolved)),
            ApprovalTapReaction::Silent);
  EXPECT_EQ(ReactionToResumeError(ApprovalResumeErrorCode(ApprovalResumeError::TurnBusy)),
            ApprovalTapReaction::BusyElsewhere);
  for (const ApprovalResumeError gone :
       {ApprovalResumeError::NotFound, ApprovalResumeError::Superseded, ApprovalResumeError::Expired}) {
    EXPECT_EQ(ReactionToResumeError(ApprovalResumeErrorCode(gone)), ApprovalTapReaction::StaleRewrite);
  }
}

// Every tool the permission policy can stop for (meta.mutating) reads in plain words, in both languages.
TEST_P(ApprovalCardTest, EveryMutatingToolHasCopyAndNeverShowsItsName) {
  ConversationsHub hub;
  ConversationsFacade facade(hub);
  ToolRegistry registry;
  RegisterMessagingTools(registry, facade);
  RegisterSettingsTools(registry, SettingsToolPorts{});

  size_t mutating = 0;
  for (const ToolDescriptor& tool : registry.Tools()) {
    if (!tool.meta.mutating) {
      continue;
    }
    ++mutating;
    const std::string name = tool.definition.name;
    const std::string key = "chat.permission.tool." + name;
    EXPECT_NE(Tr(key), key) << "no permission copy for " << name << " (" << GetParam() << ")";

    PlannedToolCall bare;
    bare.name = name;
    const std::string described = DescribeToolCalls({bare});
    EXPECT_FALSE(described.empty());
    EXPECT_EQ(described.find(name), std::string::npos) << described;
    EXPECT_EQ(described.find("chat.permission"), std::string::npos) << "a copy key leaked: " << described;
    EXPECT_NE(described, Tr("chat.permission.tool.generic")) << name << " falls back to the generic wording";
  }
  EXPECT_GE(mutating, 15u) << "the registry lost tools; this test must still iterate them";

  // The wordings the copy picks by argument.
  for (const char* tool : {"set_appearance", "set_language", "set_group_invite_policy", "set_call_invite_policy",
                           "update_profile_nickname"}) {
    EXPECT_NE(Tr(std::string("chat.permission.tool.") + tool + ".value"),
              std::string("chat.permission.tool.") + tool + ".value");
  }
  for (const char* tool : {"set_notifications", "set_auto_renew_registration", "set_reduce_transparency",
                           "set_call_diagnostics", "set_node_enabled"}) {
    for (const char* state : {".on", ".off"}) {
      const std::string key = std::string("chat.permission.tool.") + tool + state;
      EXPECT_NE(Tr(key), key);
    }
  }
}

} // namespace pbr
