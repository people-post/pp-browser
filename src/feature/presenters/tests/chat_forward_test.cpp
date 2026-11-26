// Forwarding text and contact cards (DEV/PLAN-message-forward-text.md): which messages can be forwarded, choosing them,
// and sending them on to several chats. The presenter's decisions run against a fake of the messaging calls it makes.
#include "feature/presenters/ChatPresenter.h"
#include "feature/presenters/MessageForward.h"

#include "common/Logger.h"
#include "lib/delivery/ChatPayloadCodec.h"
#include "lib/delivery/TextSpans.h"
#include "lib/messaging/QuoteReply.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <deque>
#include <map>
#include <set>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace pbr {
namespace {

ThreadMessage TextRow(const std::string& id, const std::string& text, const int64_t order,
                      const std::string& sender = "contact-a") {
  ThreadMessage row;
  row.id = id;
  row.thread_id = "src";
  row.sender_contact_id = sender;
  row.content_type = ChatContentType::Text;
  row.text = text;
  row.display_order = order;
  row.timestamp = 1000 + order;
  row.delivery = MessageDelivery::Relayed;
  return row;
}

std::string CardJson() {
  ChatContactCardFields card;
  card.display_name = "Mia";
  card.card_json = R"({"account_id":"acct-mia","v":1})";
  return ChatPayloadCodec::ContactCardFieldsToJson(card);
}

ThreadMessage CardRow(const std::string& id, const int64_t order) {
  ThreadMessage row = TextRow(id, "Mia", order);
  row.content_type = ChatContentType::ContactCard;
  row.payload_json = CardJson();
  return row;
}

struct SendCall {
  std::string method; // "user" or "group"
  std::string thread;
  std::string text;
  std::optional<ChatContentType> content_type;
  std::optional<std::string> payload_json;
};

/** The messaging calls a forward makes, as a fake: the stored rows, what each chat is, and every send. */
struct FakeMessaging {
  std::string active = "src";
  std::map<std::string, ThreadMessage> rows;
  std::map<std::string, ForwardTargetKind> targets;
  std::string account = "acct-a";
  std::vector<SendCall> calls;
  /** Runs inside each send, with the number of sends so far (this one included). */
  std::function<void(size_t)> during_send;
  /** Message texts the messaging layer refuses in every chat. */
  std::set<std::string> refused_texts;
  /** Messaging can take a send (false: locked or torn down). */
  bool ready = true;
  /** Contacts we may no longer write to (blocked meanwhile). */
  std::set<std::string> blocked_contacts;
  /** The chats made for people who had none, and the order against the sends: "made:<contact>" / "send:<thread>". */
  std::vector<std::string> made;
  std::vector<std::string> log;

  void Add(const ThreadMessage& row) { rows[row.id] = row; }

  ForwardPorts Ports() {
    ForwardPorts ports;
    ports.active_thread = [this]() { return active; };
    ports.get_message = [this](const std::string&, const std::string& id) -> std::optional<ThreadMessage> {
      const auto it = rows.find(id);
      return it == rows.end() ? std::nullopt : std::optional<ThreadMessage>(it->second);
    };
    ports.target_of = [this](const std::string& thread) {
      const auto it = targets.find(thread);
      return it == targets.end() ? ForwardTargetKind::Unavailable : it->second;
    };
    ports.account_id = [this]() { return account; };
    ports.messaging_ready = [this]() { return ready; };
    ports.contact_can_receive = [this](const std::string& contact) { return blocked_contacts.count(contact) == 0; };
    ports.direct_thread_of = [this](const std::string& contact) {
      if (contact.rfind("c-bad", 0) == 0) {
        return std::string();
      }
      made.push_back(contact);
      log.push_back("made:" + contact);
      targets["made-" + contact] = ForwardTargetKind::Direct;
      return "made-" + contact;
    };
    ports.send_user = [this](const std::string& thread, const std::string& text, SendRelayOptions options) {
      return Record("user", thread, text, options);
    };
    ports.send_group = [this](const std::string& thread, const std::string& text, SendRelayOptions options) {
      return Record("group", thread, text, options);
    };
    return ports;
  }

  Roe<ThreadMessage> Record(const std::string& method, const std::string& thread, const std::string& text,
                            const SendRelayOptions& options) {
    calls.push_back({method, thread, text, options.content_type, options.payload_json});
    log.push_back("send:" + thread);
    if (during_send) {
      during_send(calls.size());
    }
    if (refused_texts.count(text) > 0) {
      return Error("refused");
    }
    ThreadMessage sent;
    sent.id = "sent-" + std::to_string(calls.size());
    return sent;
  }

  std::vector<std::string> TextsTo(const std::string& thread) const {
    std::vector<std::string> texts;
    for (const SendCall& call : calls) {
      if (call.thread == thread) {
        texts.push_back(call.text);
      }
    }
    return texts;
  }
};

/** A presenter wired to the fake, with the batches held until the test lets them run. */
class ChatForwardTest : public ::testing::Test {
protected:
  ChatForwardTest() {
    presenter.SetForwardPortsForTest(chats.Ports());
    presenter.SetForwardPostForTest([this](std::function<void()> task) { posted.push_back(std::move(task)); });
    chats.targets = {{"bob", ForwardTargetKind::Direct},
                     {"carol", ForwardTargetKind::Direct},
                     {"club", ForwardTargetKind::Group},
                     {"dave", ForwardTargetKind::Direct}};
  }

  /** Runs the posted batches until none is left; returns how many ran. */
  size_t Drain() {
    size_t ran = 0;
    while (!posted.empty()) {
      auto task = std::move(posted.front());
      posted.pop_front();
      task();
      ++ran;
    }
    return ran;
  }

  /** Chooses `ids` (the first starts it) and forwards them to `targets`; the result once the batches have run. */
  std::optional<ForwardResult> Forward(const std::vector<std::string>& ids, std::vector<std::string> targets) {
    EXPECT_TRUE(presenter.BeginForwardSelection(ids.front()));
    for (size_t i = 1; i < ids.size(); ++i) {
      EXPECT_EQ(presenter.ToggleForwardSelection(ids[i]), ChatPresenter::ForwardTick::Selected);
    }
    std::optional<ForwardResult> result;
    presenter.ForwardSelected(std::move(targets), [&result](const ForwardResult& r) { result = r; });
    Drain();
    return result;
  }

  void AddTexts(const size_t count) {
    for (size_t i = 1; i <= count; ++i) {
      chats.Add(TextRow("m" + std::to_string(i), "text " + std::to_string(i), static_cast<int64_t>(i)));
    }
  }

  ChatPresenter presenter;
  FakeMessaging chats;
  std::deque<std::function<void()>> posted;
};

TEST_F(ChatForwardTest, AiThreadTextIsForwardable) {
  chats.active = "ai-chat";
  chats.Add(TextRow("question", "what is a mesh network?", 1, kLocalSelfContactId));
  chats.rows["question"].delivery = MessageDelivery::Local;
  chats.Add(TextRow("answer", "A mesh is ...", 2, kAiAssistantContactId));
  ThreadMessage action = TextRow("action", "Opened settings", 3, kLocalSelfContactId);
  action.generation = kLocalActionGeneration;
  chats.Add(action);
  ThreadMessage with_picture = TextRow("picture-question", "[Image] what is this?", 4, kLocalSelfContactId);
  with_picture.ai_image_json = R"({"content_hash":"ab","mime":"image/png","width":1,"height":1})";
  chats.Add(with_picture);

  EXPECT_TRUE(presenter.CanForward("question"));
  EXPECT_TRUE(presenter.CanForward("answer"));
  EXPECT_FALSE(presenter.CanForward("action")) << "an app action's row never leaves the device";
  EXPECT_FALSE(presenter.CanForward("picture-question")) << "its picture is an attachment (the second part)";
  EXPECT_FALSE(presenter.CanForward("no-such-message"));
}

TEST_F(ChatForwardTest, TextAndCardsCanBeForwardedAttachmentsAndLocalActionsCannot) {
  const ThreadMessage text = TextRow("t", "hello", 1);
  EXPECT_TRUE(CanForwardMessage(text)) << "text";
  ThreadMessage quote = TextRow("q", ComposeQuoteReply("yes", "are you coming?"), 2);
  EXPECT_TRUE(CanForwardMessage(quote)) << "a quote reply (its quote is part of the text)";
  ThreadMessage edited = TextRow("e", "after", 3, kLocalSelfContactId);
  edited.payload_json = R"({"edited_at":5,"edited_by":"x"})";
  EXPECT_TRUE(CanForwardMessage(edited)) << "an edited message";
  EXPECT_TRUE(CanForwardMessage(CardRow("c", 4))) << "a contact card";

  ThreadMessage attachment = TextRow("a", "photo.jpg", 5);
  attachment.content_type = ChatContentType::Attachment;
  EXPECT_FALSE(CanForwardMessage(attachment)) << "attachment (picture, video, file, voice): the second part";
  ThreadMessage action = TextRow("l", "Opened settings", 6, kLocalSelfContactId);
  action.generation = kLocalActionGeneration;
  EXPECT_FALSE(CanForwardMessage(action)) << "local_action";
  ThreadMessage system = TextRow("s", "Mia joined", 7);
  system.content_type = ChatContentType::System;
  EXPECT_FALSE(CanForwardMessage(system)) << "system line (also a call record)";
  ThreadMessage annotation = TextRow("n", "", 8);
  annotation.content_type = ChatContentType::Annotation;
  EXPECT_FALSE(CanForwardMessage(annotation)) << "annotation";
  ThreadMessage crypto = TextRow("x", "0.1 ETH", 9);
  crypto.content_type = ChatContentType::CryptoTx;
  EXPECT_FALSE(CanForwardMessage(crypto)) << "crypto line";
  ThreadMessage unsupported = TextRow("u", "update to view", 10);
  unsupported.content_type = ChatContentType::Unsupported;
  EXPECT_FALSE(CanForwardMessage(unsupported)) << "unsupported";
  ThreadMessage deleted = TextRow("d", "", 11);
  deleted.content_type = ChatContentType::Deleted;
  EXPECT_FALSE(CanForwardMessage(deleted)) << "tombstone";
  EXPECT_FALSE(CanForwardMessage(TextRow("empty", "", 12))) << "a text with nothing in it";
  ThreadMessage queued_action = TextRow("qa", "Opened settings", 13, kLocalSelfContactId);
  queued_action.generation = kLocalActionGeneration;
  queued_action.delivery = MessageDelivery::Pending;
  EXPECT_FALSE(CanForwardMessage(queued_action)) << "a queued local_action stays out";
  ThreadMessage queued_attachment = TextRow("qp", "photo.jpg", 14, kLocalSelfContactId);
  queued_attachment.content_type = ChatContentType::Attachment;
  queued_attachment.delivery = MessageDelivery::Pending;
  EXPECT_FALSE(CanForwardMessage(queued_attachment)) << "a queued attachment stays out";
}

TEST_F(ChatForwardTest, MyOwnQueuedAndFailedTextAndCardsCanBeForwarded) {
  ThreadMessage queued = TextRow("queued", "on its way", 1, kLocalSelfContactId);
  queued.delivery = MessageDelivery::Pending;
  ThreadMessage failed = TextRow("failed", "did not go", 2, kLocalSelfContactId);
  failed.delivery = MessageDelivery::Failed;
  ThreadMessage sent = TextRow("sent", "went out", 3, kLocalSelfContactId);
  sent.delivery = MessageDelivery::Relayed;
  ThreadMessage queued_card = CardRow("queued-card", 4);
  queued_card.sender_contact_id = kLocalSelfContactId;
  queued_card.delivery = MessageDelivery::Pending;
  for (const ThreadMessage* row : {&queued, &failed, &sent, &queued_card}) {
    chats.Add(*row);
  }
  EXPECT_TRUE(presenter.CanForward("queued"));
  EXPECT_TRUE(presenter.CanForward("failed"));
  EXPECT_TRUE(presenter.CanForward("queued-card"));
  EXPECT_TRUE(presenter.CanForward("sent"));

  const auto result = Forward({"queued"}, {"bob"});
  ASSERT_TRUE(result);
  ASSERT_EQ(chats.calls.size(), 1u);
  EXPECT_EQ(chats.calls[0].text, "on its way");
}

TEST_F(ChatForwardTest, ALocalOnlyOwnRowIsForwardedLikeAnyText) {
  ThreadMessage local = TextRow("local", "kept here", 1, kLocalSelfContactId);
  local.delivery = MessageDelivery::Local;
  chats.Add(local);
  EXPECT_TRUE(presenter.CanForward("local"));
  const auto result = Forward({"local"}, {"bob"});
  ASSERT_TRUE(result);
  ASSERT_EQ(chats.calls.size(), 1u);
  EXPECT_EQ(chats.calls[0].text, "kept here");
}

TEST_F(ChatForwardTest, ACardIAmNotAbleToResendCannotBeForwarded) {
  // A card goes out as stored, never rebuilt from the contact it names (so a person who is not a contact, or was
  // deleted or blocked, still forwards). One whose stored card does not read as a card has nothing to send.
  ThreadMessage empty_payload = CardRow("no-payload", 1);
  empty_payload.payload_json.clear();
  ThreadMessage broken_payload = CardRow("broken", 2);
  broken_payload.payload_json = "{not json";
  ThreadMessage no_card = CardRow("no-card", 3);
  no_card.payload_json = R"({"display_name":"Mia"})";
  for (const ThreadMessage* row : {&empty_payload, &broken_payload, &no_card}) {
    chats.Add(*row);
    EXPECT_FALSE(presenter.CanForward(row->id)) << row->id;
  }
  chats.Add(CardRow("good", 4));
  EXPECT_TRUE(presenter.CanForward("good"));
}

TEST_F(ChatForwardTest, ACardIsSentAgainAsStoredToAChatAndToAGroup) {
  chats.Add(CardRow("card", 1));
  const auto result = Forward({"card"}, {"bob", "club"});
  ASSERT_TRUE(result);
  ASSERT_EQ(chats.calls.size(), 2u);
  for (const SendCall& call : chats.calls) {
    EXPECT_EQ(call.content_type, ChatContentType::ContactCard);
    EXPECT_EQ(call.payload_json, CardJson()) << "the stored card, byte for byte";
    EXPECT_EQ(call.text, "Mia");
  }
}

TEST_F(ChatForwardTest, MentionsAreDroppedWhenForwarded) {
  // A group text with a mention keeps its marked ranges in payload_json; a forward is the words only (WIRE_SCHEMAS.md:
  // a copy in another chat drops them).
  const std::string words = "hello @Mia and everyone";
  TextSpan mention;
  mention.kind = static_cast<uint8_t>(TextSpanKind::Mention);
  mention.start = 6;
  mention.end = 10;
  mention.account_id = "acct-mia";
  ThreadMessage row = TextRow("mentioning", words, 1);
  row.payload_json = text_spans::ToPayloadJson(words, {mention});
  ASSERT_FALSE(text_spans::FromPayloadJson(row.payload_json).empty()) << "the fixture does carry a mention";
  chats.Add(row);
  const auto result = Forward({"mentioning"}, {"club", "bob"});
  ASSERT_TRUE(result);
  ASSERT_EQ(chats.calls.size(), 2u);
  for (const SendCall& call : chats.calls) {
    EXPECT_EQ(call.text, words);
    EXPECT_FALSE(call.payload_json) << "no marked ranges travel";
    EXPECT_FALSE(call.content_type);
  }
}

TEST_F(ChatForwardTest, AnEditedMessageForwardsItsCurrentText) {
  ThreadMessage row = TextRow("edited", "see you at 8", 1, kLocalSelfContactId);
  chats.Add(row);
  ASSERT_TRUE(presenter.BeginForwardSelection("edited"));
  // Edited after it was ticked: the row in the store reads the new words by the time the forward sends.
  chats.rows["edited"].text = "see you at 9";
  chats.rows["edited"].payload_json = R"({"edited_at":2000,"edited_by":"edit-row"})";
  std::optional<ForwardResult> result;
  presenter.ForwardSelected({"bob"}, [&result](const ForwardResult& r) { result = r; });
  Drain();
  ASSERT_TRUE(result);
  ASSERT_EQ(chats.calls.size(), 1u);
  EXPECT_EQ(chats.calls[0].text, "see you at 9");
  EXPECT_FALSE(chats.calls[0].payload_json) << "the edit mark does not travel";
}

TEST_F(ChatForwardTest, ForwardKeepsOrderInEveryTarget) {
  AddTexts(3);
  // Ticked out of order: every chat still gets them as they stand in the chat.
  ASSERT_TRUE(presenter.BeginForwardSelection("m3"));
  ASSERT_EQ(presenter.ToggleForwardSelection("m1"), ChatPresenter::ForwardTick::Selected);
  ASSERT_EQ(presenter.ToggleForwardSelection("m2"), ChatPresenter::ForwardTick::Selected);
  presenter.ForwardSelected({"bob", "club"}, nullptr);
  Drain();
  const std::vector<std::string> expected{"text 1", "text 2", "text 3"};
  EXPECT_EQ(chats.TextsTo("bob"), expected);
  EXPECT_EQ(chats.TextsTo("club"), expected);
}

TEST_F(ChatForwardTest, EachTargetIsFilledCompletelyBeforeTheNext) {
  AddTexts(5);
  // 5 messages to 4 chats is 20 sends: more than one batch, so the order is held across batches.
  const auto result = Forward({"m1", "m2", "m3", "m4", "m5"}, {"bob", "carol", "club", "dave"});
  ASSERT_TRUE(result);
  ASSERT_EQ(chats.calls.size(), 20u);
  const std::vector<std::string> order{"bob", "carol", "club", "dave"};
  for (size_t i = 0; i < chats.calls.size(); ++i) {
    EXPECT_EQ(chats.calls[i].thread, order[i / 5]) << "send " << i;
    EXPECT_EQ(chats.calls[i].text, "text " + std::to_string(i % 5 + 1)) << "send " << i;
  }
  EXPECT_EQ(result->forwarded, 20u);
}

TEST_F(ChatForwardTest, AOneToOneTargetUsesSendUserMessageAndAGroupUsesSendGroupMessage) {
  AddTexts(1);
  const auto result = Forward({"m1"}, {"bob", "club"});
  ASSERT_TRUE(result);
  ASSERT_EQ(chats.calls.size(), 2u);
  EXPECT_EQ(chats.calls[0].method, "user");
  EXPECT_EQ(chats.calls[0].thread, "bob");
  EXPECT_EQ(chats.calls[1].method, "group");
  EXPECT_EQ(chats.calls[1].thread, "club");
}

TEST_F(ChatForwardTest, AMessageRetractedWhileChoosingIsSkippedAndCounted) {
  AddTexts(3);
  ASSERT_TRUE(presenter.BeginForwardSelection("m1"));
  presenter.ToggleForwardSelection("m2");
  presenter.ToggleForwardSelection("m3");
  chats.rows.erase("m2"); // retracted while the recipients were being chosen
  chats.rows["m3"].content_type = ChatContentType::Deleted;
  std::optional<ForwardResult> result;
  presenter.ForwardSelected({"bob"}, [&result](const ForwardResult& r) { result = r; });
  Drain();
  ASSERT_TRUE(result);
  EXPECT_EQ(result->skipped_messages, 2u);
  EXPECT_EQ(result->forwarded, 1u);
  EXPECT_FALSE(result->nothing_sent);
  EXPECT_EQ(chats.TextsTo("bob"), std::vector<std::string>{"text 1"});
}

TEST_F(ChatForwardTest, ATargetThatCanNoLongerBeWrittenToIsSkippedAndCounted) {
  AddTexts(2);
  chats.targets["carol"] = ForwardTargetKind::Unavailable; // blocked, removed from the group, left
  const auto result = Forward({"m1", "m2"}, {"bob", "carol", "club"});
  ASSERT_TRUE(result);
  EXPECT_EQ(result->skipped_targets, 1u);
  EXPECT_EQ(result->forwarded, 4u);
  EXPECT_TRUE(chats.TextsTo("carol").empty()) << "that chat gets nothing";
  EXPECT_EQ(chats.TextsTo("bob").size(), 2u);
  EXPECT_EQ(chats.TextsTo("club").size(), 2u);
}

TEST_F(ChatForwardTest, AMessageRefusedInEveryChatIsCountedOnceNotOncePerChat) {
  AddTexts(3);
  chats.refused_texts = {"text 2"};
  chats.targets["erin"] = ForwardTargetKind::Direct;
  const auto result = Forward({"m1", "m2", "m3"}, {"bob", "carol", "dave", "erin"});
  ASSERT_TRUE(result);
  EXPECT_EQ(result->forwarded, 8u);
  EXPECT_EQ(result->send_errors, 1u) << "messages that failed, not sends that failed";
  EXPECT_EQ(result->skipped_messages, 0u);
}

TEST_F(ChatForwardTest, NothingSentReportsFailure) {
  AddTexts(2);
  chats.targets["bob"] = ForwardTargetKind::Unavailable;
  const auto no_target = Forward({"m1", "m2"}, {"bob"});
  ASSERT_TRUE(no_target);
  EXPECT_TRUE(no_target->nothing_sent);
  EXPECT_EQ(no_target->skipped_targets, 1u);

  ASSERT_TRUE(presenter.BeginForwardSelection("m1"));
  chats.rows.clear();
  std::optional<ForwardResult> result;
  presenter.ForwardSelected({"carol"}, [&result](const ForwardResult& r) { result = r; });
  Drain();
  ASSERT_TRUE(result);
  EXPECT_TRUE(result->nothing_sent);
  EXPECT_EQ(result->skipped_messages, 1u);
  EXPECT_TRUE(chats.calls.empty());
}

TEST_F(ChatForwardTest, SelectionStopsAtThirty) {
  AddTexts(31);
  ASSERT_TRUE(presenter.BeginForwardSelection("m1"));
  for (size_t i = 2; i <= 30; ++i) {
    ASSERT_EQ(presenter.ToggleForwardSelection("m" + std::to_string(i)), ChatPresenter::ForwardTick::Selected) << i;
  }
  EXPECT_EQ(presenter.ForwardSelectedIds().size(), 30u);
  EXPECT_EQ(presenter.ToggleForwardSelection("m31"), ChatPresenter::ForwardTick::LimitReached);
  EXPECT_EQ(presenter.ForwardSelectedIds().size(), 30u);
  EXPECT_EQ(presenter.ToggleForwardSelection("m1"), ChatPresenter::ForwardTick::Deselected);
  EXPECT_EQ(presenter.ToggleForwardSelection("m31"), ChatPresenter::ForwardTick::Selected) << "room again";
  chats.rows["m5"].content_type = ChatContentType::Attachment;
  EXPECT_EQ(presenter.ToggleForwardSelection("m5"), ChatPresenter::ForwardTick::Deselected);
  EXPECT_EQ(presenter.ToggleForwardSelection("m5"), ChatPresenter::ForwardTick::NotForwardable);
}

TEST_F(ChatForwardTest, TargetsStopAtNine) {
  AddTexts(1);
  std::vector<std::string> ten;
  std::vector<std::string> nine;
  for (int i = 1; i <= 10; ++i) {
    const std::string id = "chat-" + std::to_string(i);
    chats.targets[id] = ForwardTargetKind::Direct;
    ten.push_back(id);
    if (i <= 9) {
      nine.push_back(id);
    }
  }
  const auto too_many = Forward({"m1"}, ten);
  ASSERT_TRUE(too_many);
  EXPECT_TRUE(too_many->nothing_sent);
  EXPECT_TRUE(chats.calls.empty()) << "a tenth chat is refused before anything is sent";

  const auto ok = Forward({"m1"}, nine);
  ASSERT_TRUE(ok);
  EXPECT_EQ(ok->forwarded, 9u);
  EXPECT_FALSE(ok->nothing_sent);
}

TEST_F(ChatForwardTest, SignOutMidBatchStopsTheBatchAndCallsNoSendAfterwards) {
  AddTexts(5);
  chats.during_send = [this](const size_t sends) {
    if (sends == 7) {
      presenter.ResetForNewProfile(); // signing out, in the middle of a send
    }
  };
  bool told = false;
  ASSERT_TRUE(presenter.BeginForwardSelection("m1"));
  for (const char* id : {"m2", "m3", "m4", "m5"}) {
    presenter.ToggleForwardSelection(id);
  }
  presenter.ForwardSelected({"bob", "carol", "club"}, [&told](const ForwardResult&) { told = true; });
  Drain();
  EXPECT_EQ(chats.calls.size(), 7u) << "the seventh send was under way; nothing after it";
  EXPECT_FALSE(presenter.Forwarding());
  EXPECT_FALSE(told) << "nobody is told by a screen that is gone";
  const size_t at_stop = chats.calls.size();
  EXPECT_EQ(Drain(), 0u);
  EXPECT_EQ(chats.calls.size(), at_stop);
  EXPECT_TRUE(presenter.TakeForwardInterrupted()) << "the next chat entry says only part was forwarded";
  EXPECT_FALSE(presenter.TakeForwardInterrupted()) << "once";
}

TEST_F(ChatForwardTest, TheInterruptedNoticeIsOnlyForTheAccountItHappenedUnder) {
  AddTexts(3);
  chats.during_send = [this](const size_t sends) {
    if (sends == 2) {
      presenter.ResetForNewProfile(); // signing out in the middle of a send
    }
  };
  ASSERT_TRUE(presenter.BeginForwardSelection("m1"));
  presenter.ToggleForwardSelection("m2");
  presenter.ForwardSelected({"bob", "carol"}, nullptr);
  Drain();
  chats.account = "acct-b"; // another account signs in on this device
  EXPECT_FALSE(presenter.TakeForwardInterrupted()) << "a different account never hears of it";
  chats.account = "acct-a"; // the first one signs in again
  EXPECT_TRUE(presenter.TakeForwardInterrupted());
  EXPECT_FALSE(presenter.TakeForwardInterrupted()) << "once";
}

TEST_F(ChatForwardTest, LockMidBatchStopsTheBatchLikeSignOut) {
  // Locking clears the keys a send needs (ProfileSecretsEngine::Lock -> IdentityStore::ClearDek), so a batch does not
  // go on through a lock: the shell stops it (AbortForward) and no send is made after that.
  AddTexts(4);
  ASSERT_TRUE(presenter.BeginForwardSelection("m1"));
  presenter.ToggleForwardSelection("m2");
  presenter.ToggleForwardSelection("m3");
  presenter.ToggleForwardSelection("m4");
  bool told = false;
  presenter.ForwardSelected({"bob", "carol", "club"}, [&told](const ForwardResult&) { told = true; });
  ASSERT_EQ(posted.size(), 1u);
  posted.front()(); // the first batch: ten of the twelve sends
  posted.pop_front();
  EXPECT_EQ(chats.calls.size(), 10u);
  presenter.AbortForward(); // the vault locks between two batches
  EXPECT_EQ(Drain(), 1u) << "the queued batch runs and finds it was stopped";
  EXPECT_EQ(chats.calls.size(), 10u) << "no send after the lock";
  EXPECT_FALSE(told);
  EXPECT_TRUE(presenter.TakeForwardInterrupted());
}

TEST_F(ChatForwardTest, LockSignOutOrThreadDeletedLeavesSelectionMode) {
  AddTexts(2);
  ASSERT_TRUE(presenter.BeginForwardSelection("m1"));
  presenter.AbortForward(); // lock
  EXPECT_FALSE(presenter.ForwardChoosing());
  EXPECT_TRUE(presenter.ForwardSelectedIds().empty());

  ASSERT_TRUE(presenter.BeginForwardSelection("m1"));
  presenter.ResetForNewProfile(); // sign-out, or another account or backend
  EXPECT_FALSE(presenter.ForwardChoosing());

  ASSERT_TRUE(presenter.BeginForwardSelection("m1"));
  presenter.ForgetThread("another-chat");
  EXPECT_TRUE(presenter.ForwardChoosing()) << "another chat's deletion changes nothing";
  presenter.ForgetThread("src"); // the chat is deleted
  EXPECT_FALSE(presenter.ForwardChoosing());

  ASSERT_TRUE(presenter.BeginForwardSelection("m1"));
  presenter.SelectThread(
      "another-chat"); // a different chat opens (no messaging bound: it is refused, the choosing still ends)
  EXPECT_FALSE(presenter.ForwardChoosing());
  EXPECT_FALSE(presenter.TakeForwardInterrupted()) << "nothing was being sent";
}

Thread DirectThread(const std::string& id, const std::string& peer, const int64_t updated_at = 0) {
  Thread thread;
  thread.id = id;
  thread.kind = ThreadKind::Direct;
  thread.peer_identity_value = peer;
  thread.updated_at = updated_at;
  return thread;
}

ForwardTargetFacts SavedContact() {
  ForwardTargetFacts facts;
  facts.saved_contact = true;
  facts.my_account = "acct-me";
  facts.support_account = "acct-support";
  return facts;
}

TEST(ForwardTargetKindTest, OnlyASavedUnblockedContactOrAGroupWeMayPostInIsATarget) {
  EXPECT_EQ(ClassifyForwardTarget(DirectThread("t1", "acct-bob"), SavedContact()), ForwardTargetKind::Direct);

  Thread ai;
  ai.kind = ThreadKind::Ai;
  EXPECT_EQ(ClassifyForwardTarget(ai, SavedContact()), ForwardTargetKind::Unavailable) << "an AI conversation";

  EXPECT_EQ(ClassifyForwardTarget(DirectThread("t2", "acct-me"), SavedContact()), ForwardTargetKind::Unavailable)
      << "ourselves";
  EXPECT_EQ(ClassifyForwardTarget(DirectThread("t3", "acct-support"), SavedContact()), ForwardTargetKind::Unavailable)
      << "the PP support chat";
  ForwardTargetFacts no_support = SavedContact();
  no_support.support_account.clear();
  EXPECT_EQ(ClassifyForwardTarget(DirectThread("t4", ""), no_support), ForwardTargetKind::Direct)
      << "an empty id never matches an empty support account";

  ForwardTargetFacts blocked = SavedContact();
  blocked.blocked = true;
  EXPECT_EQ(ClassifyForwardTarget(DirectThread("t5", "acct-bob"), blocked), ForwardTargetKind::Unavailable);
  ForwardTargetFacts stranger = SavedContact();
  stranger.saved_contact = false;
  EXPECT_EQ(ClassifyForwardTarget(DirectThread("t6", "acct-bob"), stranger), ForwardTargetKind::Unavailable);

  Thread group;
  group.kind = ThreadKind::Group;
  group.group_id = "g1";
  ForwardTargetFacts member;
  member.in_group = true;
  EXPECT_EQ(ClassifyForwardTarget(group, member), ForwardTargetKind::Group);
  ForwardTargetFacts left;
  EXPECT_EQ(ClassifyForwardTarget(group, left), ForwardTargetKind::Unavailable) << "removed or left";
  ForwardTargetFacts ended = member;
  ended.group_dissolved = true;
  EXPECT_EQ(ClassifyForwardTarget(group, ended), ForwardTargetKind::Unavailable);
}

ForwardTarget Chat(const std::string& thread, const std::string& contact, const std::string& title, const bool group,
                   const int64_t updated_at) {
  ForwardTarget target;
  target.thread_id = thread;
  target.contact_id = contact;
  target.title = title;
  target.group = group;
  target.updated_at = updated_at;
  return target;
}

TEST(ForwardTargetKindTest, TheListIsRecentChatsThenOtherPeopleThenOtherGroupsEachOnce) {
  std::vector<ForwardTarget> all;
  for (int i = 0; i < 10; ++i) { // ten chats; the eight most recent lead
    all.push_back(
        Chat("t" + std::to_string(i), "c" + std::to_string(i), "Person " + std::to_string(i), false, 100 + i));
  }
  all.push_back(Chat("", "c-new", "Zed", false, 0));         // a contact with no chat yet
  all.push_back(Chat("", "c3", "Person 3 again", false, 0)); // c3 has a chat: not listed twice
  all.push_back(Chat("g-old", "", "Alpha club", true, 5));
  all.push_back(Chat("g-new", "", "Beta club", true, 500)); // the most recent chat of all
  all.push_back(Chat("t0", "c0", "Person 0", false, 100));  // the same chat twice
  const std::vector<ForwardTarget> ordered = OrderForwardTargets(all);

  std::vector<std::string> keys;
  for (const ForwardTarget& target : ordered) {
    keys.push_back(target.Key());
  }
  const std::vector<std::string> expected = {"g-new", "t9", "t8", "t7",
                                             "t6",    "t5", "t4", "t3",            // recent chats
                                             "t0",    "t1", "t2", "contact:c-new", // the other people, by name
                                             "g-old"};                             // the other groups
  EXPECT_EQ(keys, expected);
}

TEST_F(ChatForwardTest, TheListOffersNoAiNoSupportNoSelfNoBlockedAndNoGroupWeLeft) {
  ForwardCandidate bob{DirectThread("t-bob", "acct-bob", 10), "bob", "Bob", "", SavedContact()};
  ForwardCandidate ai;
  ai.thread.id = "t-ai";
  ai.thread.kind = ThreadKind::Ai;
  ai.title = "New chat";
  ForwardCandidate support{DirectThread("t-support", "acct-support", 9), "support", "PP support", "", SavedContact()};
  ForwardCandidate me{DirectThread("t-me", "acct-me", 8), "me", "Me", "", SavedContact()};
  ForwardCandidate blocked{DirectThread("t-blocked", "acct-bad", 7), "bad", "Bad", "", SavedContact()};
  blocked.facts.blocked = true;
  ForwardCandidate left;
  left.thread.id = "t-left";
  left.thread.kind = ThreadKind::Group;
  left.thread.group_id = "g-left";
  left.title = "Left club";
  ForwardCandidate club;
  club.thread.id = "t-club";
  club.thread.kind = ThreadKind::Group;
  club.thread.group_id = "g-club";
  club.thread.updated_at = 20;
  club.title = "Club";
  club.facts.in_group = true;
  ForwardPorts ports = chats.Ports();
  ports.candidates = [=]() { return std::vector<ForwardCandidate>{bob, ai, support, me, blocked, left, club}; };
  presenter.SetForwardPortsForTest(ports);

  std::vector<std::string> keys;
  for (const ForwardTarget& target : presenter.ForwardTargets()) {
    keys.push_back(target.Key());
  }
  EXPECT_EQ(keys, (std::vector<std::string>{"t-club", "t-bob"}));
}

TEST_F(ChatForwardTest, TheSupportChatIsRefusedAtSendTimeToo) {
  AddTexts(1);
  ForwardPorts ports = chats.Ports();
  ports.target_of = [](const std::string& thread) {
    // What the facade's port does for a chat: its facts, then ClassifyForwardTarget.
    const Thread t = DirectThread(thread, thread == "t-support" ? "acct-support" : "acct-bob");
    return ClassifyForwardTarget(t, SavedContact());
  };
  presenter.SetForwardPortsForTest(ports);
  const std::optional<ForwardResult> result = Forward({"m1"}, {"t-support", "t-bob"});
  ASSERT_TRUE(result);
  EXPECT_EQ(result->skipped_targets, 1u);
  EXPECT_EQ(chats.TextsTo("t-support").size(), 0u) << "nothing goes to the support chat";
  EXPECT_EQ(chats.TextsTo("t-bob").size(), 1u);
}

TEST_F(ChatForwardTest, APersonWithNoChatYetGetsOneRightBeforeTheSendsToThem) {
  AddTexts(2);
  std::optional<ForwardResult> result;
  ASSERT_TRUE(presenter.BeginForwardSelection("m1"));
  presenter.ToggleForwardSelection("m2");
  presenter.ForwardSelectedTo({Chat("bob", "c1", "A", false, 1), Chat("", "c-ok", "B", false, 0)},
                              [&result](const ForwardResult& r) { result = r; });
  EXPECT_TRUE(chats.made.empty()) << "nothing is made while the forward is only asked for";
  Drain();
  ASSERT_TRUE(result);
  EXPECT_EQ(result->forwarded, 4u);
  EXPECT_EQ(chats.log,
            (std::vector<std::string>{"send:bob", "send:bob", "made:c-ok", "send:made-c-ok", "send:made-c-ok"}))
      << "the chat is made after the earlier chats are done and right before the sends to it";
}

TEST_F(ChatForwardTest, ARefusedForwardMakesNoChatForAnyone) {
  AddTexts(1);
  // More than nine chats: refused before anything.
  std::vector<ForwardTarget> ten;
  for (int i = 0; i < 10; ++i) {
    ten.push_back(Chat("", "c-new" + std::to_string(i), "P", false, 0));
  }
  ASSERT_TRUE(presenter.BeginForwardSelection("m1"));
  std::optional<ForwardResult> refused;
  presenter.ForwardSelectedTo(ten, [&refused](const ForwardResult& r) { refused = r; });
  Drain();
  ASSERT_TRUE(refused);
  EXPECT_TRUE(refused->nothing_sent);
  EXPECT_TRUE(chats.made.empty()) << "ten chats: no chat is made for any of them";

  // One forward still sending: a second is refused and makes nothing either.
  ASSERT_TRUE(presenter.BeginForwardSelection("m1"));
  presenter.ForwardSelectedTo({Chat("bob", "c1", "A", false, 1)}, nullptr); // runs on the posted batch
  chats.Add(TextRow("m2", "text 2", 2));
  ASSERT_FALSE(presenter.BeginForwardSelection("m2")) << "no new choosing while one is sending";
  Drain();
  ASSERT_TRUE(presenter.BeginForwardSelection("m1")); // the first is done now
  presenter.ForwardSelectedTo({Chat("bob", "c1", "A", false, 1), Chat("bob", "c1", "A", false, 1)}, nullptr);
  Drain();

  // A contact blocked while choosing: skipped, no chat made.
  chats.made.clear();
  chats.blocked_contacts = {"c-blocked"};
  ASSERT_TRUE(presenter.BeginForwardSelection("m1"));
  std::optional<ForwardResult> result;
  presenter.ForwardSelectedTo({Chat("", "c-blocked", "Blocked", false, 0), Chat("bob", "c1", "A", false, 1)},
                              [&result](const ForwardResult& r) { result = r; });
  Drain();
  ASSERT_TRUE(result);
  EXPECT_EQ(result->skipped_targets, 1u);
  EXPECT_TRUE(chats.made.empty()) << "no chat for someone blocked meanwhile";
}

TEST_F(ChatForwardTest, TwoPeopleWhoseChatsCouldNotBeMadeAreTwoChatsNotReached) {
  AddTexts(1);
  ASSERT_TRUE(presenter.BeginForwardSelection("m1"));
  std::optional<ForwardResult> result;
  presenter.ForwardSelectedTo(
      {Chat("", "c-bad1", "A", false, 0), Chat("", "c-bad2", "B", false, 0), Chat("bob", "c1", "C", false, 1)},
      [&result](const ForwardResult& r) { result = r; });
  Drain();
  ASSERT_TRUE(result);
  EXPECT_EQ(result->skipped_targets, 2u) << "each person is counted";
  EXPECT_EQ(result->forwarded, 1u);
}

TEST_F(ChatForwardTest, NoSendIsMadeOnceMessagingIsNotReadyEvenWithABatchAlreadyQueued) {
  AddTexts(3);
  ASSERT_TRUE(presenter.BeginForwardSelection("m1"));
  presenter.ForwardSelected({"bob"}, nullptr); // the first batch is queued, not run
  ASSERT_EQ(posted.size(), 1u);
  chats.ready = false; // locked or torn down before the queued batch gets its turn
  EXPECT_EQ(Drain(), 1u);
  EXPECT_TRUE(chats.calls.empty()) << "the queued batch sent nothing";
  EXPECT_FALSE(presenter.Forwarding());
}

TEST_F(ChatForwardTest, MessagingGoingAwayMidBatchStopsAtOnceAndSaysOnlyPartWentOut) {
  AddTexts(5);
  chats.during_send = [this](const size_t sends) {
    if (sends == 3) {
      chats.ready = false;
    }
  };
  ASSERT_TRUE(presenter.BeginForwardSelection("m1"));
  for (const char* id : {"m2", "m3", "m4", "m5"}) {
    presenter.ToggleForwardSelection(id);
  }
  bool told = false;
  presenter.ForwardSelected({"bob", "carol", "club"}, [&told](const ForwardResult&) { told = true; });
  Drain();
  EXPECT_EQ(chats.calls.size(), 3u) << "no send after messaging stopped being ready";
  EXPECT_FALSE(told);
  EXPECT_TRUE(presenter.TakeForwardInterrupted());
}

TEST_F(ChatForwardTest, AForwardIsRefusedWhileMessagingIsNotReady) {
  AddTexts(1);
  ASSERT_TRUE(presenter.BeginForwardSelection("m1"));
  chats.ready = false;
  std::optional<ForwardResult> result;
  presenter.ForwardSelected({"bob"}, [&result](const ForwardResult& r) { result = r; });
  Drain();
  ASSERT_TRUE(result);
  EXPECT_TRUE(result->nothing_sent);
  EXPECT_TRUE(chats.calls.empty());
}

struct MetricsCapture : logging::Handler {
  void emit(logging::Level, const std::string&, const std::string& message) override {
    std::lock_guard lock(mu);
    lines.push_back(message);
  }
  std::vector<std::string> Lines() {
    std::lock_guard lock(mu);
    return lines;
  }
  std::mutex mu;
  std::vector<std::string> lines;
};

TEST_F(ChatForwardTest, TheForwardMetricCarriesFourCountsAndNoIds) {
  auto capture = std::make_shared<MetricsCapture>();
  logging::getLogger("Metrics").addHandler(capture);
  chats.active = "secret-source-chat";
  chats.targets = {{"secret-target-1", ForwardTargetKind::Direct},
                   {"secret-target-2", ForwardTargetKind::Group},
                   {"secret-target-3", ForwardTargetKind::Unavailable}};
  chats.Add(TextRow("secret-message-1", "words nobody may log", 1));
  chats.Add(TextRow("secret-message-2", "more words", 2));
  chats.Add(TextRow("secret-message-3", "gone soon", 3));
  ASSERT_TRUE(presenter.BeginForwardSelection("secret-message-1"));
  presenter.ToggleForwardSelection("secret-message-2");
  presenter.ToggleForwardSelection("secret-message-3");
  chats.rows.erase("secret-message-3");
  presenter.ForwardSelected({"secret-target-1", "secret-target-2", "secret-target-3"}, nullptr);
  Drain();

  std::string line;
  for (const std::string& candidate : capture->Lines()) {
    if (candidate.find("event=chat.forward") != std::string::npos) {
      EXPECT_TRUE(line.empty()) << "one line per forward";
      line = candidate;
    }
  }
  ASSERT_FALSE(line.empty());
  EXPECT_NE(line.find("messages=3"), std::string::npos) << line;
  EXPECT_NE(line.find("targets=3"), std::string::npos) << line;
  EXPECT_NE(line.find("skipped_messages=1"), std::string::npos) << line;
  EXPECT_NE(line.find("skipped_targets=1"), std::string::npos) << line;
  EXPECT_EQ(line.find("secret"), std::string::npos) << "no chat or message id, no words: " << line;
}

} // namespace
} // namespace pbr
