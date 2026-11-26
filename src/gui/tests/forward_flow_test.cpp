// The forward flow from a long press to the pill (DEV/PLAN-message-forward-text.md, step 4): ChatController cannot be
// built in a unit test (a started messaging hub, the shell), so what it does for a forward lives in ForwardFlow, run
// here with the real ChatPresenter and the real "Forward to" list over fakes of the messaging calls and of the picker.
// ChatController's wiring of the flow is read from its source (forward_flow_wiring_test.cpp).
#include "gui/chat/ForwardFlow.h"

#include "feature/presenters/ChatPresenter.h"
#include "foundation/i18n/LocalizationService.h"
#include "lib/delivery/ChatPayloadCodec.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <deque>
#include <memory>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace pbr {
namespace {

ThreadMessage Text(const std::string& id, const std::string& text, const int64_t order) {
  ThreadMessage row;
  row.id = id;
  row.thread_id = "src";
  row.sender_contact_id = "contact-a";
  row.content_type = ChatContentType::Text;
  row.text = text;
  row.display_order = order;
  row.timestamp = 1000 + order;
  row.delivery = MessageDelivery::Relayed;
  return row;
}

ForwardCandidate Person(const std::string& thread, const std::string& contact, const std::string& title,
                        const int64_t updated_at) {
  ForwardCandidate candidate;
  candidate.thread.id = thread;
  candidate.thread.kind = ThreadKind::Direct;
  candidate.thread.peer_identity_value = "acct-" + contact;
  candidate.thread.updated_at = updated_at;
  candidate.contact_id = contact;
  candidate.title = title;
  candidate.facts.saved_contact = true;
  candidate.facts.my_account = "acct-me";
  return candidate;
}

struct Send {
  std::string method;
  std::string thread;
  std::string text;
};

class ForwardFlowTest : public ::testing::Test {
protected:
  ForwardFlowTest() {
    for (int i = 1; i <= 3; ++i) {
      rows["m" + std::to_string(i)] = Text("m" + std::to_string(i), "text " + std::to_string(i), i);
    }
    candidates = {Person("t-bob", "bob", "Bob", 30), Person("t-cy", "cy", "Cy", 20)};
    ForwardPorts ports;
    ports.active_thread = []() { return std::string("src"); };
    ports.get_message = [this](const std::string&, const std::string& id) -> std::optional<ThreadMessage> {
      const auto it = rows.find(id);
      return it == rows.end() ? std::nullopt : std::optional<ThreadMessage>(it->second);
    };
    ports.target_of = [this](const std::string& thread) {
      for (const ForwardCandidate& candidate : candidates) {
        if (candidate.thread.id == thread) {
          return ClassifyForwardTarget(candidate.thread, candidate.facts);
        }
      }
      return ForwardTargetKind::Unavailable;
    };
    ports.candidates = [this]() { return candidates; };
    ports.account_id = []() { return std::string("acct-me"); };
    ports.send_user = [this](const std::string& thread, const std::string& text, SendRelayOptions) {
      sends.push_back({"user", thread, text});
      ThreadMessage sent;
      sent.id = "sent-" + std::to_string(sends.size());
      return Roe<ThreadMessage>(sent);
    };
    ports.send_group = ports.send_user;
    presenter.SetForwardPortsForTest(ports);
    presenter.SetForwardPostForTest([this](std::function<void()> task) { posted.push_back(std::move(task)); });

    ForwardFlowPorts screen;
    screen.open_picker = [this](PeoplePickerChooseOptions options, PeoplePickerChooseDone done,
                                std::function<void()> cancel) {
      picker_open = true;
      ++pickers_opened;
      picker_options = std::move(options);
      picker_done = std::move(done);
      picker_cancel = std::move(cancel);
    };
    screen.close_picker = [this]() {
      picker_open = false;
      ++picker_closed_by_host;
    };
    screen.pill = [this](const std::string& message) { pills.push_back(message); };
    screen.changed = [this]() { ++redraws; };
    flow = std::make_unique<ForwardFlow>(presenter, screen);
  }

  void Drain() {
    while (!posted.empty()) {
      auto task = std::move(posted.front());
      posted.pop_front();
      task();
    }
  }

  /** The picker's confirm: it closes itself, then hands the choice back. */
  void PickerSends(const std::vector<std::string>& keys) {
    std::vector<ForwardTarget> chosen;
    for (const ForwardTarget& target : picker_options.list()) {
      if (std::find(keys.begin(), keys.end(), target.Key()) != keys.end()) {
        chosen.push_back(target);
      }
    }
    picker_open = false;
    picker_done(std::move(chosen));
  }

  ChatPresenter presenter;
  std::unique_ptr<ForwardFlow> flow;
  std::map<std::string, ThreadMessage> rows;
  std::vector<ForwardCandidate> candidates;
  std::vector<Send> sends;
  std::deque<std::function<void()>> posted;
  std::vector<std::string> pills;
  bool picker_open = false;
  int pickers_opened = 0;
  int picker_closed_by_host = 0;
  int redraws = 0;
  PeoplePickerChooseOptions picker_options;
  PeoplePickerChooseDone picker_done;
  std::function<void()> picker_cancel;
};

TEST_F(ForwardFlowTest, ForwardRunsFromSelectionThroughPickerToSendAndClosesEverything) {
  flow->BeginSelecting("m1"); // the menu's Forward on m1
  ASSERT_TRUE(flow->Selecting());
  flow->Tap("m2");
  EXPECT_EQ(flow->CountLabel(), Tr("chat.forward.selected_count", {{"n", "2"}}));
  ASSERT_TRUE(flow->CanNext());
  flow->Next(); // the bottom button
  ASSERT_TRUE(picker_open);
  EXPECT_EQ(picker_options.title, Tr("chat.forward.pick_title"));
  EXPECT_EQ(picker_options.preview, Tr("chat.forward.preview_count", {{"n", "2"}}));
  EXPECT_EQ(picker_options.max_targets, 9u);
  EXPECT_FALSE(flow->CanNext()) << "the button waits while the picker is up";

  PickerSends({"t-bob", "t-cy"});
  Drain();

  ASSERT_EQ(sends.size(), 4u) << "two messages to each of two chats";
  const std::vector<Send> expected = {
      {"user", "t-bob", "text 1"}, {"user", "t-bob", "text 2"}, {"user", "t-cy", "text 1"}, {"user", "t-cy", "text 2"}};
  for (size_t i = 0; i < expected.size(); ++i) {
    EXPECT_EQ(sends[i].thread, expected[i].thread) << i;
    EXPECT_EQ(sends[i].text, expected[i].text) << i;
  }
  EXPECT_FALSE(picker_open) << "the picker is closed";
  EXPECT_FALSE(flow->Selecting()) << "choosing is over";
  EXPECT_FALSE(presenter.Forwarding());
  EXPECT_EQ(pills, (std::vector<std::string>{Tr("chat.forward.done")}));
}

TEST_F(ForwardFlowTest, ClosingThePickerLeavesTheMessagesTicked) {
  flow->BeginSelecting("m1");
  flow->Next();
  picker_open = false;
  picker_cancel();
  EXPECT_TRUE(flow->Selecting());
  EXPECT_TRUE(flow->Ticked("m1"));
  EXPECT_TRUE(flow->CanNext()) << "the button works again";
  EXPECT_TRUE(sends.empty());
}

TEST_F(ForwardFlowTest, LockWhileThePickerIsOpenClosesThePickerAndLeavesSelection) {
  flow->BeginSelecting("m1");
  flow->Tap("m2");
  flow->Next();
  ASSERT_TRUE(picker_open);
  flow->Stop(); // the app locked
  EXPECT_FALSE(picker_open);
  EXPECT_EQ(picker_closed_by_host, 1);
  EXPECT_FALSE(flow->Selecting());
  EXPECT_TRUE(presenter.ForwardSelectedIds().empty());
  // An answer from the picker that was still on its way does nothing.
  picker_done({});
  PickerSends({"t-bob"});
  Drain();
  EXPECT_TRUE(sends.empty());
  EXPECT_TRUE(pills.empty());
}

TEST_F(ForwardFlowTest, SignOutWhileThePickerIsOpenClosesIt) {
  flow->BeginSelecting("m1");
  flow->Next();
  ASSERT_TRUE(picker_open);
  flow->Stop(); // ChatController stops the flow, then resets the presenter for the next profile
  presenter.ResetForNewProfile();
  EXPECT_FALSE(picker_open);
  EXPECT_EQ(picker_closed_by_host, 1);
  EXPECT_FALSE(flow->Selecting());
  EXPECT_FALSE(presenter.TakeForwardInterrupted()) << "nothing had been sent";
}

TEST_F(ForwardFlowTest, TheNextChatEntryTellsOnlyPartWasForwardedOnce) {
  for (int i = 4; i <= 14; ++i) { // enough messages that one batch (ten sends) does not finish the forward
    rows["m" + std::to_string(i)] = Text("m" + std::to_string(i), "text " + std::to_string(i), i);
  }
  flow->BeginSelecting("m1");
  for (int i = 2; i <= 14; ++i) {
    flow->Tap("m" + std::to_string(i));
  }
  flow->Next();
  PickerSends({"t-bob", "t-cy"});
  ASSERT_EQ(posted.size(), 1u);
  posted.front()(); // ten sends are out, a second batch is waiting
  posted.pop_front();
  flow->Stop(); // the app locks between two batches
  Drain();
  EXPECT_EQ(sends.size(), 10u) << "nothing is sent after the lock";
  flow->OnChatEntered();
  EXPECT_EQ(pills, (std::vector<std::string>{Tr("chat.forward.interrupted")}));
  flow->OnChatEntered();
  EXPECT_EQ(pills.size(), 1u) << "once";
}

TEST_F(ForwardFlowTest, TappingARowThatCannotBeForwardedShowsThePill) {
  ThreadMessage picture = Text("pic", "", 9);
  picture.content_type = ChatContentType::Attachment;
  rows["pic"] = picture;
  flow->BeginSelecting("m1");
  flow->Tap("pic");
  EXPECT_EQ(pills, (std::vector<std::string>{Tr("chat.forward.cannot")}));
  EXPECT_FALSE(flow->Ticked("pic"));
  EXPECT_EQ(presenter.ForwardSelectedIds().size(), 1u);
}

TEST_F(ForwardFlowTest, AMessageThatCannotBeForwardedDoesNotStartChoosing) {
  ThreadMessage picture = Text("pic", "", 9);
  picture.content_type = ChatContentType::Attachment;
  rows["pic"] = picture;
  flow->BeginSelecting("pic");
  EXPECT_FALSE(flow->Selecting());
  EXPECT_EQ(pills, (std::vector<std::string>{Tr("chat.forward.cannot")}));
}

TEST_F(ForwardFlowTest, TheThirtyFirstMessageShowsTheLimitPill) {
  for (int i = 4; i <= 40; ++i) {
    rows["m" + std::to_string(i)] = Text("m" + std::to_string(i), "t", i);
  }
  flow->BeginSelecting("m1");
  for (int i = 2; i <= 30; ++i) {
    flow->Tap("m" + std::to_string(i));
  }
  EXPECT_TRUE(pills.empty());
  flow->Tap("m31");
  EXPECT_EQ(pills, (std::vector<std::string>{Tr("chat.forward.limit_messages")}));
  EXPECT_EQ(presenter.ForwardSelectedIds().size(), 30u);
}

TEST_F(ForwardFlowTest, TheBottomButtonIsOffWithNothingTicked) {
  flow->BeginSelecting("m1");
  flow->Tap("m1"); // unticked again
  EXPECT_TRUE(flow->Selecting());
  EXPECT_FALSE(flow->CanNext());
  flow->Next();
  EXPECT_EQ(pickers_opened, 0);
}

TEST_F(ForwardFlowTest, CancelEndsChoosingAndTheChatBeingDeletedEndsItToo) {
  flow->BeginSelecting("m1");
  flow->Cancel();
  EXPECT_FALSE(flow->Selecting());
  flow->BeginSelecting("m1");
  flow->OnThreadGone("another-chat");
  EXPECT_TRUE(flow->Selecting()) << "another chat going does not matter";
  flow->OnThreadGone("src");
  EXPECT_FALSE(flow->Selecting());
}

// The chat being chosen from is deleted: the presenter forgets the selection first (ForgetThread), the controller's
// ForgetComposerExtras tells the flow after; the flow must still redraw, or the next chat shows the old "N selected" bar.
TEST_F(ForwardFlowTest, ADeletedSourceChatEndsTheBarEvenWhenThePresenterForgotTheSelectionFirst) {
  flow->BeginSelecting("m1");
  flow->Tap("m2");
  const int before = redraws;
  presenter.ForgetThread("src"); // CloseChatWith -> presenter_.CloseThread -> ForgetThread
  EXPECT_FALSE(flow->Selecting());
  flow->OnThreadGone("src"); // ForgetComposerExtras
  EXPECT_GT(redraws, before) << "the bars are drawn again";
  EXPECT_FALSE(flow->CanNext());
  flow->OnThreadGone("elsewhere");
  EXPECT_FALSE(flow->Selecting());
}

TEST_F(ForwardFlowTest, ASecondForwardWhileOneIsStillSendingSaysItIsStillSending) {
  flow->BeginSelecting("m1");
  flow->Next();
  PickerSends({"t-bob"});
  ASSERT_TRUE(presenter.Forwarding());
  pills.clear();
  flow->BeginSelecting("m2");
  EXPECT_EQ(pills, (std::vector<std::string>{Tr("chat.send.still_trying")})) << "the reason, not 'cannot be forwarded'";
  EXPECT_FALSE(flow->Selecting());
}

TEST_F(ForwardFlowTest, ARefusedPickerLeavesTheForwardButtonWorking) {
  // The host could not open it: the cancel runs at once (a picker that refuses answers like a closed one).
  ForwardFlowPorts screen;
  screen.open_picker = [](PeoplePickerChooseOptions, PeoplePickerChooseDone, std::function<void()> cancel) {
    cancel();
  };
  screen.changed = [this]() { ++redraws; };
  ForwardFlow refusing(presenter, screen);
  refusing.BeginSelecting("m1");
  refusing.Next();
  EXPECT_TRUE(refusing.CanNext()) << "the Forward button is not dead";
}

// "Forward" on one message (WeChat style): the picker opens at once with that message, and the chat never shows the
// selection bars; "Forward several" is the old multi-select (BeginSelecting).
TEST_F(ForwardFlowTest, ForwardOnOneMessageOpensThePickerAtOnceWithNoSelectionBars) {
  flow->BeginSingle("m2");
  ASSERT_TRUE(picker_open);
  EXPECT_EQ(pickers_opened, 1);
  EXPECT_EQ(picker_options.title, Tr("chat.forward.pick_title"));
  EXPECT_EQ(picker_options.preview, "text 2") << "the usual strip for that one message";
  EXPECT_FALSE(flow->Selecting()) << "no bars, no ticks";
  EXPECT_FALSE(flow->CanNext());
  flow->Tap("m1"); // a stray tap does not start choosing
  EXPECT_FALSE(flow->Selecting());
  EXPECT_EQ(presenter.ForwardSelectedIds().size(), 1u);
}

TEST_F(ForwardFlowTest, CancellingTheSingleForwardPickerLeavesNoSelectionBehind) {
  flow->BeginSingle("m1");
  ASSERT_TRUE(picker_open);
  picker_open = false;
  picker_cancel();
  EXPECT_FALSE(flow->Selecting());
  EXPECT_FALSE(presenter.ForwardChoosing());
  EXPECT_TRUE(presenter.ForwardSelectedIds().empty());
  EXPECT_TRUE(sends.empty());
  EXPECT_TRUE(pills.empty());
  flow->BeginSingle("m3"); // and the next forward works
  EXPECT_EQ(pickers_opened, 2);
}

TEST_F(ForwardFlowTest, SingleForwardSendsExactlyThatMessageToTheChosenChats) {
  flow->BeginSingle("m2");
  PickerSends({"t-bob", "t-cy"});
  Drain();
  ASSERT_EQ(sends.size(), 2u);
  EXPECT_EQ(sends[0].thread, "t-bob");
  EXPECT_EQ(sends[0].text, "text 2");
  EXPECT_EQ(sends[1].thread, "t-cy");
  EXPECT_EQ(sends[1].text, "text 2");
  EXPECT_FALSE(flow->Selecting());
  EXPECT_FALSE(presenter.ForwardChoosing());
  EXPECT_FALSE(presenter.Forwarding());
  EXPECT_EQ(pills, (std::vector<std::string>{Tr("chat.forward.done")}));
}

TEST_F(ForwardFlowTest, ForwardSeveralEntersSelectionModeWithTheMessageTicked) {
  flow->BeginSelecting("m2"); // the menu's "Forward several"
  EXPECT_TRUE(flow->Selecting());
  EXPECT_TRUE(flow->Ticked("m2"));
  EXPECT_EQ(pickers_opened, 0) << "the picker waits for the bottom button";
  flow->Tap("m3");
  ASSERT_TRUE(flow->CanNext());
  flow->Next();
  ASSERT_TRUE(picker_open);
  EXPECT_EQ(picker_options.preview, Tr("chat.forward.preview_count", {{"n", "2"}}));
  PickerSends({"t-bob"});
  Drain();
  ASSERT_EQ(sends.size(), 2u);
  EXPECT_FALSE(flow->Selecting());
}

TEST_F(ForwardFlowTest, LockOrSignOutWhileTheSingleForwardPickerIsOpenClosesItAndSelectsNothing) {
  flow->BeginSingle("m1");
  ASSERT_TRUE(picker_open);
  flow->Stop(); // the app locked
  EXPECT_FALSE(picker_open);
  EXPECT_EQ(picker_closed_by_host, 1);
  EXPECT_FALSE(flow->Selecting());
  EXPECT_TRUE(presenter.ForwardSelectedIds().empty());
  PickerSends({"t-bob"}); // a late answer does nothing
  Drain();
  EXPECT_TRUE(sends.empty());
  EXPECT_TRUE(pills.empty());

  flow->BeginSingle("m1"); // sign-out: stop, then the presenter is reset for the next profile
  ASSERT_TRUE(picker_open);
  flow->Stop();
  presenter.ResetForNewProfile();
  EXPECT_FALSE(picker_open);
  EXPECT_FALSE(flow->Selecting());
  EXPECT_TRUE(presenter.ForwardSelectedIds().empty());
}

TEST_F(ForwardFlowTest, ARefusedPickerInTheSingleForwardLeavesTheChatUntouched) {
  ForwardFlowPorts screen;
  screen.open_picker = [](PeoplePickerChooseOptions, PeoplePickerChooseDone, std::function<void()> cancel) {
    cancel(); // the host could not open it
  };
  screen.changed = [this]() { ++redraws; };
  ForwardFlow refusing(presenter, screen);
  refusing.BeginSingle("m1");
  EXPECT_FALSE(refusing.Selecting());
  EXPECT_FALSE(presenter.ForwardChoosing());
  EXPECT_TRUE(presenter.ForwardSelectedIds().empty());
  refusing.BeginSelecting("m1"); // and the multi-select still starts
  EXPECT_TRUE(refusing.Selecting());
}

class ForwardResultTextTest : public ::testing::Test {
protected:
  void SetUp() override { ASSERT_TRUE(LocalizationService::Instance().LoadFromAssets(PP_BROWSER_ASSETS_DIR)); }
  void TearDown() override { LocalizationService::Instance().SetPreferredLanguage("en"); }

  static ForwardResult Sent() {
    ForwardResult result;
    result.forwarded = 4;
    return result;
  }
};

TEST_F(ForwardResultTextTest, SaysWhatWentOutAndWhatDidNotInEnglish) {
  LocalizationService::Instance().SetPreferredLanguage("en");
  EXPECT_EQ(ForwardResultText(Sent()), "Forwarded");
  ForwardResult messages = Sent();
  messages.skipped_messages = 2;
  EXPECT_EQ(ForwardResultText(messages), "Forwarded. 2 could not be forwarded.");
  ForwardResult chats = Sent();
  chats.skipped_targets = 3;
  EXPECT_EQ(ForwardResultText(chats), "Forwarded. 3 chats could not be reached.");
  ForwardResult both = Sent();
  both.skipped_messages = 1;
  both.skipped_targets = 2;
  EXPECT_EQ(ForwardResultText(both), "Forwarded. 1 could not be forwarded. 2 chats could not be reached.");
  ForwardResult nothing;
  nothing.nothing_sent = true;
  EXPECT_EQ(ForwardResultText(nothing), "Forwarding failed");
}

TEST_F(ForwardResultTextTest, SaysWhatWentOutAndWhatDidNotInChinese) {
  LocalizationService::Instance().SetPreferredLanguage("zh-Hans");
  EXPECT_EQ(ForwardResultText(Sent()), "已转发");
  ForwardResult messages = Sent();
  messages.skipped_messages = 2;
  EXPECT_EQ(ForwardResultText(messages), "已转发，2 条转发失败");
  ForwardResult chats = Sent();
  chats.skipped_targets = 3;
  EXPECT_EQ(ForwardResultText(chats), "已转发，3 个聊天转发失败");
  ForwardResult both = Sent();
  both.skipped_messages = 1;
  both.skipped_targets = 2;
  EXPECT_EQ(ForwardResultText(both), "已转发，1 条转发失败，2 个聊天转发失败");
  ForwardResult nothing;
  nothing.nothing_sent = true;
  EXPECT_EQ(ForwardResultText(nothing), "转发失败");
}

} // namespace
} // namespace pbr
