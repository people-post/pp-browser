// A failed message's red mark offers to update the contact's card (or our own) and send the message again
// (DEV/PLAN-send-failure-guidance.md): every outcome of the two choices, driven through fake ports.
#include "gui/chat/DeliveryNoticeText.h"
#include "gui/chat/SendFailureFix.h"

#include "foundation/i18n/LocalizationService.h"

#include <gtest/gtest.h>

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace pbr {
namespace {

using Devices = ContactRefreshResult::Devices;
using Directory = ContactRefreshResult::Directory;

ContactRefreshResult Result(const Devices devices, const Directory directory, const bool changed = false,
                            const bool keys_rejected = false) {
  ContactRefreshResult result;
  result.devices = devices;
  result.directory = directory;
  result.name_changed = changed;
  result.directory_keys_rejected = keys_rejected;
  return result;
}

class SendFailureFixTest : public ::testing::Test {
protected:
  void SetUp() override {
    ASSERT_TRUE(LocalizationService::Instance().LoadFromAssets(PP_BROWSER_ASSETS_DIR));
    LocalizationService::Instance().SetPreferredLanguage("en");
    SendFailureFix::Ports ports;
    ports.refresh_contact = [this](const std::string& contact_id,
                                   std::function<void(const ContactRefreshResult&)> done) {
      refreshed_contacts.push_back(contact_id);
      if (refuse_refresh) {
        return false;
      }
      refresh_done.push_back(std::move(done));
      return true;
    };
    ports.resend = [this](const std::string& thread_id, const std::string& message_id) {
      resent.push_back(thread_id + "/" + message_id);
    };
    ports.update_my_card = [this](std::function<void(bool)> done) {
      ++my_card_updates;
      my_card_done.push_back(std::move(done));
    };
    ports.poll_ok_pending = [this]() { return recovering; };
    ports.progress = [this](const std::string& text) { progress.push_back(text); };
    ports.say = [this](const std::string& text) { said.push_back(text); };
    fix.SetPorts(std::move(ports));
  }
  void TearDown() override { LocalizationService::Instance().SetPreferredLanguage("en"); }

  /** The refresh answers `result` (the oldest one asked). */
  void Answer(const ContactRefreshResult& result) {
    ASSERT_FALSE(refresh_done.empty());
    auto done = std::move(refresh_done.front());
    refresh_done.erase(refresh_done.begin());
    done(result);
  }
  /** Our card's update ends (the oldest one asked). */
  void MyCardEnds(const bool ok) {
    ASSERT_FALSE(my_card_done.empty());
    auto done = std::move(my_card_done.front());
    my_card_done.erase(my_card_done.begin());
    done(ok);
  }
  static std::string Words(const char* key) { return Tr(key); }

  SendFailureFix fix;
  std::vector<std::string> refreshed_contacts;
  std::vector<std::function<void(const ContactRefreshResult&)>> refresh_done;
  bool refuse_refresh = false;
  std::vector<std::string> resent;
  int my_card_updates = 0;
  std::vector<std::function<void(bool)>> my_card_done;
  bool recovering = true;
  std::vector<std::string> progress;
  std::vector<std::string> said;
};

TEST_F(SendFailureFixTest, TheirCardIsRefreshedForTheRightContactWithAProgressNote) {
  fix.UpdateTheirCard("t1", "m1", "contact:bob", true);
  EXPECT_EQ(refreshed_contacts, (std::vector<std::string>{"contact:bob"}));
  EXPECT_EQ(progress, (std::vector<std::string>{"Updating their card…"}));
  EXPECT_TRUE(resent.empty()) << "nothing is sent before the answer";
}

TEST_F(SendFailureFixTest, FoundAndUpdatedResendsOnce) {
  fix.UpdateTheirCard("t1", "m1", "contact:bob", true);
  Answer(Result(Devices::Answered, Directory::Answered, /*changed=*/true));
  EXPECT_EQ(resent, (std::vector<std::string>{"t1/m1"}));
  EXPECT_TRUE(said.empty());
}

TEST_F(SendFailureFixTest, FoundAndUpToDateResendsOnce) {
  fix.UpdateTheirCard("t1", "m1", "contact:bob", true);
  Answer(Result(Devices::NotAsked, Directory::Answered));
  EXPECT_EQ(resent, (std::vector<std::string>{"t1/m1"}));
  fix.UpdateTheirCard("t1", "m2", "contact:bob", true);
  Answer(Result(Devices::Answered, Directory::NotFound)); // a device knew them: found
  EXPECT_EQ(resent, (std::vector<std::string>{"t1/m1", "t1/m2"}));
}

TEST_F(SendFailureFixTest, ThreeRedMessagesTappedOneAfterAnotherAreSentOnceEach) {
  for (const char* id : {"m1", "m2", "m3"}) {
    fix.UpdateTheirCard("t1", id, "contact:bob", true);
    Answer(Result(Devices::Answered, Directory::Answered));
  }
  EXPECT_EQ(refreshed_contacts.size(), 3u);
  EXPECT_EQ(resent, (std::vector<std::string>{"t1/m1", "t1/m2", "t1/m3"}));
}

TEST_F(SendFailureFixTest, NotFoundSendsNothingAndSaysSo) {
  fix.UpdateTheirCard("t1", "m1", "contact:bob", true);
  Answer(Result(Devices::NotReached, Directory::NotFound));
  EXPECT_TRUE(resent.empty());
  EXPECT_EQ(said, (std::vector<std::string>{
                      "Couldn't find them. They may not have updated PP yet — let them know another way."}));
}

TEST_F(SendFailureFixTest, UnreachableSendsNothingAndSaysTryLater) {
  fix.UpdateTheirCard("t1", "m1", "contact:bob", true);
  Answer(Result(Devices::NotReached, Directory::Unreachable));
  fix.UpdateTheirCard("t1", "m2", "contact:bob", true);
  Answer(Result(Devices::NotReached, Directory::NotAsked)); // their devices are offline, nobody else to ask
  EXPECT_TRUE(resent.empty());
  ASSERT_EQ(said.size(), 2u);
  EXPECT_EQ(said[0], Words("contacts.refresh.unreachable"));
  EXPECT_EQ(said[1], Words("contacts.refresh.unreachable"));
  LocalizationService::Instance().SetPreferredLanguage("zh-Hans");
  EXPECT_EQ(Words("contacts.refresh.unreachable"), "现在连不上，请稍后再试。");
}

TEST_F(SendFailureFixTest, KeysThatDoNotCheckOutSendNothingAndSayTheRefreshWords) {
  fix.UpdateTheirCard("t1", "m1", "contact:bob", true);
  Answer(Result(Devices::NotAsked, Directory::Answered, /*changed=*/true, /*keys_rejected=*/true));
  EXPECT_TRUE(resent.empty());
  EXPECT_EQ(said, (std::vector<std::string>{Words("contacts.refresh.keys_rejected")}));
}

TEST_F(SendFailureFixTest, NothingToAskSaysTheCardIsIncompleteAndSendsNothing) {
  fix.UpdateTheirCard("t1", "m1", "contact:bob", true);
  Answer(Result(Devices::NotAsked, Directory::NotAsked));
  EXPECT_TRUE(resent.empty());
  EXPECT_EQ(said, (std::vector<std::string>{Words("contacts.refresh.nothing_to_ask")}));
}

TEST_F(SendFailureFixTest, AMessageThatCannotBeResentIsNotSentAndTheUserIsToldToSendItAgain) {
  fix.UpdateTheirCard("t1", "voice", "contact:bob", /*resendable=*/false);
  Answer(Result(Devices::Answered, Directory::Answered, true));
  EXPECT_TRUE(resent.empty());
  EXPECT_EQ(said, (std::vector<std::string>{"Updated. Please send it again."}));

  said.clear();
  fix.UpdateTheirCard("t1", "voice", "contact:bob", false);
  Answer(Result(Devices::NotReached, Directory::NotFound));
  EXPECT_TRUE(resent.empty());
  ASSERT_EQ(said.size(), 1u);
  EXPECT_EQ(said[0], Words("chat.send.fix.not_found")) << "never 'updated' when they were not found";
  LocalizationService::Instance().SetPreferredLanguage("zh-Hans");
  EXPECT_EQ(Words("chat.send.fix.updated_send_again"), "已更新，请再发一次。");
  EXPECT_EQ(Words("chat.send.fix.not_found"), "没有找到对方。对方可能还没有更新 PP，请用别的方式提醒他。");
}

TEST_F(SendFailureFixTest, AnAnswerAfterTheTeardownDoesNothing) {
  fix.UpdateTheirCard("t1", "m1", "contact:bob", true);
  fix.Reset();
  Answer(Result(Devices::Answered, Directory::Answered, true));
  EXPECT_TRUE(resent.empty());
  EXPECT_TRUE(said.empty());
}

TEST_F(SendFailureFixTest, MyCardIsUpdatedByB216sActionAndTheMessageIsSentAfterThePollGetsThrough) {
  fix.UpdateMyCard("t1", "m1", true);
  EXPECT_EQ(my_card_updates, 1);
  EXPECT_TRUE(resent.empty());
  MyCardEnds(true);
  EXPECT_TRUE(resent.empty()) << "the poll that gets through is still to come";
  fix.OnPollOk();
  EXPECT_EQ(resent, (std::vector<std::string>{"t1/m1"}));
  fix.OnPollOk();
  EXPECT_EQ(resent.size(), 1u) << "once";
  EXPECT_FALSE(fix.Pending());
}

TEST_F(SendFailureFixTest, WithNoRefusedPollTheRegistrationItselfReleasesTheMessage) {
  recovering = false;
  fix.UpdateMyCard("t1", "m1", true);
  MyCardEnds(true);
  EXPECT_EQ(resent, (std::vector<std::string>{"t1/m1"}));
}

TEST_F(SendFailureFixTest, ASecondTapWhileTheUpdateRunsStartsNoSecondRegistrationAndSendsEachMessageOnce) {
  fix.UpdateMyCard("t1", "m1", true);
  fix.UpdateMyCard("t1", "m1", true); // the same message again
  fix.UpdateMyCard("t1", "m2", true); // another red message
  EXPECT_EQ(my_card_updates, 1);
  MyCardEnds(true);
  fix.UpdateMyCard("t1", "m3", true); // still waiting for the poll: carried too, no new registration
  EXPECT_EQ(my_card_updates, 1);
  fix.OnPollOk();
  EXPECT_EQ(resent, (std::vector<std::string>{"t1/m1", "t1/m2", "t1/m3"}));
}

TEST_F(SendFailureFixTest, AMessageThatCannotBeResentOnlyGetsTheSendItAgainWords) {
  fix.UpdateMyCard("t1", "voice", false);
  MyCardEnds(true);
  fix.OnPollOk();
  EXPECT_TRUE(resent.empty());
  EXPECT_EQ(said, (std::vector<std::string>{"Updated. Please send it again."}));

  said.clear();
  fix.UpdateMyCard("t1", "voice", false);
  fix.UpdateMyCard("t1", "m2", true);
  MyCardEnds(true);
  fix.OnPollOk();
  EXPECT_EQ(resent, (std::vector<std::string>{"t1/m2"}));
  EXPECT_EQ(said.size(), 1u);
}

TEST_F(SendFailureFixTest, AFailedUpdateClearsWhatWaitedAndALaterPollSendsNothing) {
  fix.UpdateMyCard("t1", "m1", true);
  fix.UpdateMyCard("t1", "m2", true);
  MyCardEnds(false); // refused, no network, or the PIN was cancelled
  EXPECT_FALSE(fix.Pending());
  fix.OnPollOk();
  EXPECT_TRUE(resent.empty());
  // An unrelated later renewal that gets through (a poll) sends nothing either.
  fix.OnPollOk();
  EXPECT_TRUE(resent.empty());
  // A new tap starts a fresh update.
  fix.UpdateMyCard("t1", "m3", true);
  EXPECT_EQ(my_card_updates, 2);
  MyCardEnds(true);
  fix.OnPollOk();
  EXPECT_EQ(resent, (std::vector<std::string>{"t1/m3"}));
}

TEST_F(SendFailureFixTest, ThePollOfAnUpdateThatWasNeverAskedForSendsNothing) {
  fix.OnPollOk();
  fix.UpdateMyCard("t1", "m1", true); // asked, but not registered yet
  fix.OnPollOk();
  EXPECT_TRUE(resent.empty()) << "a poll before our registration worked is not its point";
  MyCardEnds(true);
  fix.OnPollOk();
  EXPECT_EQ(resent, (std::vector<std::string>{"t1/m1"}));
}

TEST_F(SendFailureFixTest, TheTeardownClearsWhatWaitsAndIgnoresTheLateAnswer) {
  fix.UpdateMyCard("t1", "m1", true);
  MyCardEnds(true);
  fix.Reset();
  EXPECT_FALSE(fix.Pending());
  fix.OnPollOk();
  EXPECT_TRUE(resent.empty());

  fix.UpdateMyCard("t1", "m2", true);
  fix.Reset();
  MyCardEnds(true); // the session before the teardown answers late
  fix.OnPollOk();
  EXPECT_TRUE(resent.empty());
  EXPECT_FALSE(fix.Pending());
  // The new session starts clean.
  fix.UpdateMyCard("t1", "m3", true);
  EXPECT_EQ(my_card_updates, 3);
}

TEST_F(SendFailureFixTest, TheDialogsSayWhatThePlanSays) {
  SendFixDialog dialog = UpdateTheirCardDialog(true);
  EXPECT_EQ(dialog.title, "This message wasn't sent");
  EXPECT_EQ(dialog.body, "You're connected, but the server can't find them. Update their card and try again.");
  EXPECT_EQ(dialog.ok_label, "Update and resend");
  EXPECT_EQ(UpdateTheirCardDialog(false).ok_label, "Update their card");
  dialog = UpdateMyCardDialog();
  EXPECT_EQ(dialog.title, Words("chat.reconnect.alert_title"));
  EXPECT_EQ(dialog.body, Words("chat.reconnect.alert_body"));
  EXPECT_EQ(dialog.ok_label, Words("chat.reconnect.alert_ok")) << "B216's three texts";
  LocalizationService::Instance().SetPreferredLanguage("zh-Hans");
  dialog = UpdateTheirCardDialog(true);
  EXPECT_EQ(dialog.title, "这条消息没有发出去");
  EXPECT_EQ(dialog.body, "你已经连上了，但在服务器上找不到对方。先更新一下对方的名片再试。");
  EXPECT_EQ(dialog.ok_label, "更新并重发");
  EXPECT_EQ(UpdateTheirCardDialog(false).ok_label, "更新对方的名片");
  EXPECT_EQ(Words("chat.send.fix.updating"), "正在更新对方的名片…");
}

TEST_F(SendFailureFixTest, TheReasonWordsFollowHowATapGoesOn) {
  EXPECT_EQ(SendFailureText(SendFailure::RecipientNotFound, SendAdvice::UpdateTheirCardAndResend),
            "The server can't find them, so this message wasn't sent. Tap the red mark to update their card and resend.");
  EXPECT_EQ(SendFailureText(SendFailure::RecipientNotFound, SendAdvice::UpdateTheirCardOnly),
            "The server can't find them, so this message wasn't sent. Tap the red mark to update their card.");
  EXPECT_EQ(SendFailureText(SendFailure::RecipientNotFound), "The server can't find them, so this message wasn't sent.");
  EXPECT_EQ(SendFailureText(SendFailure::SenderNotRegistered),
            "People can't find you right now, so this message wasn't sent. Update your card first.");
  EXPECT_EQ(SendFailureReasonText(SendFailure::RecipientNotFound, SendMarkTap::Refusal::None,
                                  SendAdvice::UpdateTheirCardAndResend),
            SendFailureText(SendFailure::RecipientNotFound, SendAdvice::UpdateTheirCardAndResend));
  LocalizationService::Instance().SetPreferredLanguage("zh-Hans");
  EXPECT_EQ(SendFailureText(SendFailure::RecipientNotFound, SendAdvice::UpdateTheirCardAndResend),
            "服务器上找不到对方，这条消息没有发出去。点红色叹号可以更新对方的名片并重发。");
  EXPECT_EQ(SendFailureText(SendFailure::RecipientNotFound, SendAdvice::UpdateTheirCardOnly),
            "服务器上找不到对方，这条消息没有发出去。点红色叹号可以更新对方的名片。");
  EXPECT_EQ(SendFailureText(SendFailure::RecipientNotFound), "服务器上找不到对方，这条消息没有发出去。");
  EXPECT_EQ(SendFailureText(SendFailure::SenderNotRegistered), "别人现在找不到你，这条消息没有发出去。请先更新你的名片。");
  // Every other reason's words ignore the advice.
  EXPECT_EQ(SendFailureText(SendFailure::RelayUnreachable, SendAdvice::UpdateTheirCardAndResend),
            SendFailureText(SendFailure::RelayUnreachable));
}

TEST(ClassifyRefreshTest, EachResultTypeHasOneOutcome) {
  EXPECT_EQ(ClassifyRefresh(Result(Devices::Answered, Directory::Answered, true)), RefreshOutcome::Found);
  EXPECT_EQ(ClassifyRefresh(Result(Devices::NotAsked, Directory::Answered)), RefreshOutcome::Found);
  EXPECT_EQ(ClassifyRefresh(Result(Devices::Answered, Directory::Unreachable)), RefreshOutcome::Found);
  EXPECT_EQ(ClassifyRefresh(Result(Devices::NotReached, Directory::NotFound)), RefreshOutcome::NotFound);
  EXPECT_EQ(ClassifyRefresh(Result(Devices::NotAsked, Directory::NotFound)), RefreshOutcome::NotFound);
  EXPECT_EQ(ClassifyRefresh(Result(Devices::NotReached, Directory::Unreachable)), RefreshOutcome::Unreachable);
  EXPECT_EQ(ClassifyRefresh(Result(Devices::NotReached, Directory::NotAsked)), RefreshOutcome::Unreachable);
  EXPECT_EQ(ClassifyRefresh(Result(Devices::NotAsked, Directory::NotAsked)), RefreshOutcome::NothingToAsk);
  EXPECT_EQ(ClassifyRefresh(Result(Devices::Answered, Directory::Answered, true, true)), RefreshOutcome::KeysRejected);
}

} // namespace
} // namespace pbr
