// Forwarding pictures, videos, files and voice messages (DEV/PLAN-message-forward-files-v3.md, step 2): which
// attachments can be forwarded, the limits checked before anything starts, and the run that sends them one at a time,
// waiting for each one's answer, in the chat's order. The presenter's decisions run against a fake of the messaging
// calls it makes, with a fake clock: nothing here sleeps.
#include "feature/presenters/ChatPresenter.h"
#include "feature/presenters/MessageForward.h"

#include "common/Logger.h"
#include "foundation/error/AppError.h"
#include "lib/delivery/ChatPayloadCodec.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace pbr {
namespace {

constexpr uint64_t kMb = 1000ULL * 1000ULL;

std::vector<uint8_t> HashOf(const uint8_t tag) {
  return std::vector<uint8_t>(32, tag);
}

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

ThreadMessage CardRow(const std::string& id, const int64_t order) {
  ChatContactCardFields card;
  card.display_name = "Mia";
  card.card_json = R"({"account_id":"acct-mia","v":1})";
  ThreadMessage row = TextRow(id, "Mia", order);
  row.content_type = ChatContentType::ContactCard;
  row.payload_json = ChatPayloadCodec::ContactCardFieldsToJson(card);
  return row;
}

/** An attachment message: `declared` is what the sender said its size is; `tag` makes its hash (and its bytes' fill). */
ThreadMessage AttachmentRow(const std::string& id, const int64_t order, const std::string& filename,
                            const std::string& mime, const uint64_t declared, const uint8_t tag,
                            const std::string& sender = "contact-a") {
  ChatAttachmentFields fields;
  fields.url = "https://relay.example/secret-url-" + id;
  fields.mime = mime;
  fields.filename = filename;
  fields.byte_length = declared;
  fields.content_hash = HashOf(tag);
  fields.content_key = std::vector<uint8_t>(32, 0xEE);
  fields.blob_nonce = std::vector<uint8_t>(24, 0xDD);
  ThreadMessage row = TextRow(id, filename, order, sender);
  row.content_type = ChatContentType::Attachment;
  row.payload_json = ChatPayloadCodec::AttachmentFieldsToJson(fields);
  return row;
}

ThreadMessage Picture(const std::string& id, const int64_t order, const uint8_t tag, const uint64_t declared = 1000) {
  return AttachmentRow(id, order, "photo-" + id + ".jpg", "image/jpeg", declared, tag);
}

struct AttachmentCall {
  std::string thread;
  OutgoingAttachment attachment;
};

/** The messaging calls a forward makes, as a fake: stored rows, what each chat is, the bytes held, every send, the clock. */
struct Fake {
  std::string active = "src";
  std::map<std::string, ThreadMessage> rows;
  std::map<std::string, ForwardTargetKind> targets;
  std::string account = "acct-a";
  bool ready = true;
  int64_t now = 1'000'000;

  /** What was done, in order: "text:<chat>:<text>", "card:<chat>", "att:<chat>:<filename>", "load:<filename-or-tag>". */
  std::vector<std::string> log;
  std::vector<AttachmentCall> attachment_calls;

  /** The bytes this device holds, by content hash; `unreadable` ones are stored but cannot be decrypted. */
  std::map<std::vector<uint8_t>, ByteVector> store;
  std::set<std::vector<uint8_t>> unreadable;
  size_t stats = 0;
  size_t loads = 0;
  size_t answered_loads = 0;
  size_t max_unanswered_loads = 0;
  size_t clock_reads = 0;
  size_t timers_set = 0;

  /** What a sent text / card / attachment looks like in the chat once stored: Relayed unless held "queued". */
  MessageDelivery text_delivery = MessageDelivery::Relayed;
  MessageDelivery attachment_delivery = MessageDelivery::Relayed;
  /** How many of the next attachment sends are refused at once by the send pace (answered inside the call). */
  int rate_limited_next = 0;
  /** Texts the messaging layer refuses. */
  std::set<std::string> refused_texts;
  size_t sent_ids = 0;

  struct Started {
    std::string thread;
    std::function<void(Roe<ThreadMessage>)> on_done;
    std::function<bool(uint64_t, uint64_t)> progress;
    std::string filename;
  };
  std::deque<Started> in_flight;

  struct Timer {
    int64_t at = 0;
    std::function<void()> task;
  };
  std::vector<Timer> timers;

  void Add(const ThreadMessage& row) { rows[row.id] = row; }
  void Hold(const ThreadMessage& row, const ByteVector& bytes) {
    Add(row);
    auto fields = ChatPayloadCodec::DecodeAttachmentJson(row.payload_json);
    store[fields->content_hash] = bytes;
  }
  void HoldFilled(const ThreadMessage& row, const size_t size = 100) {
    auto fields = ChatPayloadCodec::DecodeAttachmentJson(row.payload_json);
    Hold(row, ByteVector(size, fields->content_hash[0]));
  }

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
    ports.send_user = [this](const std::string& thread, const std::string& text, SendRelayOptions options) {
      return SendText(thread, text, options);
    };
    ports.send_group = ports.send_user;
    ports.local_bytes = [this](const std::vector<uint8_t>& hash) -> uint64_t {
      ++stats;
      const auto it = store.find(hash);
      return it == store.end() ? 0 : it->second.size() + 16;
    };
    ports.load_plaintext = [this](const std::vector<uint8_t>& hash) -> Roe<ByteVector> {
      ++loads;
      max_unanswered_loads = std::max(max_unanswered_loads, loads - answered_loads);
      log.push_back("load:" + std::to_string(hash[0]));
      const auto it = store.find(hash);
      if (it == store.end() || unreadable.count(hash) > 0) {
        ++answered_loads;
        return Error("cannot read");
      }
      return it->second;
    };
    ports.send_attachment = [this](const std::string& thread, OutgoingAttachment attachment,
                                   std::function<void(Roe<ThreadMessage>)> on_done,
                                   std::function<bool(uint64_t, uint64_t)> progress) {
      log.push_back("att:" + thread + ":" + attachment.filename);
      attachment_calls.push_back({thread, attachment});
      if (rate_limited_next > 0) {
        --rate_limited_next;
        ++answered_loads;
        on_done(AppError::Blob(Err::Blob::RateLimited, "pace")); // inside the call, as the hub does
        return;
      }
      in_flight.push_back({thread, std::move(on_done), std::move(progress), attachment.filename});
    };
    ports.now_ms = [this]() {
      ++clock_reads;
      return now;
    };
    ports.after = [this](const int64_t delay_ms, std::function<void()> task) {
      ++timers_set;
      timers.push_back({now + delay_ms, std::move(task)});
    };
    return ports;
  }

  Roe<ThreadMessage> SendText(const std::string& thread, const std::string& text, const SendRelayOptions& options) {
    log.push_back((options.content_type && *options.content_type == ChatContentType::ContactCard ? "card:" : "text:") +
                  thread + (options.content_type ? "" : ":" + text));
    if (refused_texts.count(text) > 0) {
      return Error("refused");
    }
    ThreadMessage sent;
    sent.id = "sent-" + std::to_string(++sent_ids);
    sent.thread_id = thread;
    sent.delivery = text_delivery;
    rows[sent.id] = sent;
    return sent;
  }

  /** The messaging layer answers the oldest attachment send: stored (the row comes into the chat) or refused. */
  void Answer(Roe<ThreadMessage> answer) {
    ASSERT_FALSE(in_flight.empty());
    Started started = std::move(in_flight.front());
    in_flight.pop_front();
    ++answered_loads;
    if (answer) {
      ThreadMessage stored = *answer;
      stored.id = "att-" + std::to_string(++sent_ids);
      stored.thread_id = started.thread;
      stored.delivery = attachment_delivery;
      rows[stored.id] = stored;
      started.on_done(stored);
      return;
    }
    started.on_done(answer);
  }
  void AnswerOk() { Answer(ThreadMessage{}); }

  /** Marks every queued send of ours as sent. */
  void SettleAll() {
    for (auto& [id, row] : rows) {
      if (row.delivery == MessageDelivery::Pending && row.sender_contact_id != "contact-a") {
        row.delivery = MessageDelivery::Relayed;
      }
    }
  }
};

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

class ChatForwardFilesTest : public ::testing::Test {
protected:
  ChatForwardFilesTest() {
    presenter.SetForwardPortsForTest(chats.Ports());
    presenter.SetForwardPostForTest([this](std::function<void()> task) { posted.push_back(std::move(task)); });
    presenter.SetForwardNoticeSink([this](const ChatPresenter::ForwardNotice& notice) { sink.push_back(notice); });
    chats.targets = {{"bob", ForwardTargetKind::Direct},
                     {"carol", ForwardTargetKind::Direct},
                     {"club", ForwardTargetKind::Group},
                     {"dave", ForwardTargetKind::Direct}};
  }

  /** Runs the posted tasks until none is left. */
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

  /** Moves the clock; every timer that has come due fires, then what it posted runs. */
  void Advance(const int64_t ms) {
    chats.now += ms;
    for (bool again = true; again;) {
      again = false;
      for (size_t i = 0; i < chats.timers.size(); ++i) {
        if (chats.timers[i].at <= chats.now) {
          auto task = std::move(chats.timers[i].task);
          chats.timers.erase(chats.timers.begin() + static_cast<std::ptrdiff_t>(i));
          task();
          again = true;
          break;
        }
      }
    }
    Drain();
  }

  void Choose(const std::vector<std::string>& ids) {
    ASSERT_TRUE(presenter.BeginForwardSelection(ids.front()));
    for (size_t i = 1; i < ids.size(); ++i) {
      ASSERT_EQ(presenter.ToggleForwardSelection(ids[i]), ChatPresenter::ForwardTick::Selected);
    }
  }

  /** Chooses `ids` and forwards them to `targets`; what is done at once has been done when this returns. */
  void Start(const std::vector<std::string>& ids, std::vector<std::string> targets) {
    Choose(ids);
    presenter.ForwardSelected(std::move(targets), [this](const ForwardResult& r) {
      result = r;
      ++done_calls;
    });
    Drain();
  }

  /** The messaging layer answers every attachment send in turn until the forward ends. */
  void AnswerUntilDone() {
    for (int guard = 0; guard < 100 && done_calls == 0; ++guard) {
      if (chats.in_flight.empty()) {
        Advance(kForwardSettlePollMs);
        chats.SettleAll();
        if (chats.in_flight.empty() && done_calls == 0) {
          Advance(kForwardRateLimitRetryMs);
        }
        continue;
      }
      chats.AnswerOk();
      Drain();
    }
  }

  std::vector<std::string> Attachments(const std::string& thread) const {
    std::vector<std::string> names;
    for (const AttachmentCall& call : chats.attachment_calls) {
      if (call.thread == thread) {
        names.push_back(call.attachment.filename);
      }
    }
    return names;
  }

  ChatPresenter presenter;
  Fake chats;
  std::deque<std::function<void()>> posted;
  std::optional<ForwardResult> result;
  int done_calls = 0;
  std::vector<ChatPresenter::ForwardNotice> sink;
};

TEST_F(ChatForwardFilesTest, AttachmentsCanBeForwardedFailedQueuedAndDeletedOnesCannot) {
  ThreadMessage received = Picture("received", 1, 1);
  EXPECT_TRUE(CanForwardMessage(received)) << "a picture we received";
  ThreadMessage mine = Picture("mine", 2, 2);
  mine.sender_contact_id = kLocalSelfContactId;
  EXPECT_TRUE(CanForwardMessage(mine)) << "ours, sent";
  ThreadMessage voice = AttachmentRow("voice", 3, "voice-4200.ogg", "audio/ogg", 20000, 3);
  EXPECT_TRUE(CanForwardMessage(voice)) << "a voice message";
  ThreadMessage video = AttachmentRow("video", 4, "clip.mp4", "video/mp4", 5 * kMb, 4);
  EXPECT_TRUE(CanForwardMessage(video));
  ThreadMessage file = AttachmentRow("file", 5, "notes.pdf", "application/pdf", 90000, 5);
  EXPECT_TRUE(CanForwardMessage(file));

  ThreadMessage queued = mine;
  queued.id = "queued";
  queued.delivery = MessageDelivery::Pending;
  EXPECT_FALSE(CanForwardMessage(queued)) << "ours and still queued";
  ThreadMessage failed = mine;
  failed.id = "failed";
  failed.delivery = MessageDelivery::Failed;
  EXPECT_FALSE(CanForwardMessage(failed)) << "ours and not sent";
  ThreadMessage deleted = received;
  deleted.content_type = ChatContentType::Deleted;
  EXPECT_FALSE(CanForwardMessage(deleted)) << "a tombstone";
  ThreadMessage damaged = received;
  damaged.payload_json = "not json";
  EXPECT_FALSE(CanForwardMessage(damaged)) << "its fields cannot be read";
  ThreadMessage blank = TextRow("blank", "photo.jpg", 6);
  blank.content_type = ChatContentType::Attachment;
  EXPECT_FALSE(CanForwardMessage(blank)) << "an attachment with no payload";
  ThreadMessage ai = received;
  ai.ai_image_json = R"({"content_hash":"ab","mime":"image/png","width":1,"height":1})";
  EXPECT_FALSE(CanForwardMessage(ai)) << "an AI question's picture";

  chats.Add(received);
  chats.Add(queued);
  EXPECT_TRUE(presenter.CanForward("received"));
  EXPECT_FALSE(presenter.CanForward("queued"));
  EXPECT_TRUE(presenter.BeginForwardSelection("received"))
      << "choosing may start on a picture, with or without its bytes";
}

TEST_F(ChatForwardFilesTest, ARunWithoutAttachmentsBehavesExactlyAsBefore) {
  chats.Add(TextRow("t1", "one", 1));
  chats.Add(CardRow("c1", 2));
  chats.Add(TextRow("t2", "two", 3));
  Start({"t1", "c1", "t2"}, {"bob", "club"});
  ASSERT_EQ(done_calls, 1) << "all three batches' work was done in the first posted task";
  EXPECT_EQ(result->forwarded, 6u);
  EXPECT_EQ(result->total_items, 6u);
  EXPECT_EQ(result->attachments, 0u);
  EXPECT_EQ(result->attachments_failed, 0u);
  EXPECT_EQ(result->stop, ForwardStop::None);
  EXPECT_EQ(chats.log, (std::vector<std::string>{"text:bob:one", "card:bob", "text:bob:two", "text:club:one",
                                                 "card:club", "text:club:two"}));
  EXPECT_EQ(chats.stats, 0u) << "no local-bytes stat";
  EXPECT_EQ(chats.loads, 0u);
  EXPECT_TRUE(chats.attachment_calls.empty());
  EXPECT_EQ(chats.timers_set, 0u) << "no timer, no poll";
  EXPECT_EQ(chats.clock_reads, 0u) << "the clock is not even read";
  EXPECT_FALSE(presenter.ForwardHasProgressPage());
}

TEST_F(ChatForwardFilesTest, AnAttachmentItemWaitsForItsCallbackBeforeTheNextItem) {
  chats.HoldFilled(Picture("p1", 1, 1));
  chats.HoldFilled(Picture("p2", 2, 2));
  Start({"p1", "p2"}, {"bob"});
  EXPECT_EQ(Attachments("bob"), (std::vector<std::string>{"photo-p1.jpg"})) << "only the first is under way";
  EXPECT_TRUE(presenter.Forwarding());
  EXPECT_TRUE(presenter.ForwardHasProgressPage());
  EXPECT_EQ(Drain(), 0u) << "nothing runs while it waits";
  Advance(60000);
  EXPECT_EQ(Attachments("bob").size(), 1u) << "however long it takes, the second does not start";
  EXPECT_EQ(done_calls, 0);

  const ForwardProgress mid = presenter.ForwardProgressNow();
  EXPECT_TRUE(mid.uploading);
  EXPECT_EQ(mid.done, 0u);
  EXPECT_EQ(mid.total, 2u);
  chats.in_flight.front().progress(40, 100); // the messaging layer reports the upload
  EXPECT_EQ(presenter.ForwardProgressNow().bytes_done, 40u);
  EXPECT_EQ(presenter.ForwardProgressNow().bytes_total, 100u);

  chats.AnswerOk();
  Drain();
  EXPECT_EQ(Attachments("bob"), (std::vector<std::string>{"photo-p1.jpg", "photo-p2.jpg"}));
  EXPECT_EQ(presenter.ForwardProgressNow().done, 1u);
  EXPECT_EQ(done_calls, 0);
  chats.AnswerOk();
  Drain();
  Advance(kForwardSettlePollMs);
  ASSERT_EQ(done_calls, 1);
  EXPECT_EQ(result->forwarded, 2u);
  EXPECT_FALSE(presenter.Forwarding());
}

TEST_F(ChatForwardFilesTest, TextAfterAPictureIsSentOnlyAfterThePictureFinished) {
  chats.Add(TextRow("t1", "before", 1));
  chats.HoldFilled(Picture("p", 2, 1));
  chats.Add(TextRow("t2", "after", 3));
  chats.attachment_delivery = MessageDelivery::Pending; // the picture's row is stored but still queued
  Start({"t1", "p", "t2"}, {"bob"});
  EXPECT_EQ(chats.log, (std::vector<std::string>{"text:bob:before", "load:1", "att:bob:photo-p.jpg"}))
      << "the text before it went first; the text after waits for the picture's answer";

  chats.AnswerOk();
  Drain();
  EXPECT_EQ(chats.log.size(), 3u) << "answered, but the picture's row is still queued: the text still waits";
  Advance(kForwardSettlePollMs);
  EXPECT_EQ(chats.log.size(), 3u);
  chats.SettleAll(); // the picture has left the queue
  Advance(kForwardSettlePollMs);
  EXPECT_EQ(chats.log.back(), "text:bob:after") << "after the picture's callback, and after it left the queue";
  EXPECT_EQ(chats.log.size(), 4u);
  ASSERT_EQ(done_calls, 1);
  EXPECT_EQ(result->forwarded, 3u);
}

TEST_F(ChatForwardFilesTest, APictureAfterTextIsStartedOnlyWhenTheTextIsSafelyAhead) {
  // In a group, or with a peer reached over a link, the messaging layer does not hold a later send behind a queued
  // earlier one, so the run does: the picture starts only when the text before it is no longer queued.
  chats.Add(TextRow("t", "first", 1));
  chats.HoldFilled(Picture("p", 2, 1));
  chats.text_delivery = MessageDelivery::Pending;
  Start({"t", "p"}, {"club"});
  EXPECT_EQ(chats.log, (std::vector<std::string>{"text:club:first"})) << "the text is sent, the picture is not started";
  Advance(kForwardSettlePollMs);
  Advance(kForwardSettlePollMs);
  EXPECT_TRUE(chats.attachment_calls.empty()) << "still queued: still waiting";
  EXPECT_EQ(done_calls, 0);

  chats.rows["sent-1"].delivery = MessageDelivery::Relayed; // it went out
  Advance(kForwardSettlePollMs);
  EXPECT_EQ(Attachments("club"), (std::vector<std::string>{"photo-p.jpg"})) << "now the picture goes";
  chats.AnswerOk();
  Drain();
  ASSERT_EQ(done_calls, 1);
  EXPECT_EQ(result->forwarded, 2u);

  // A text that failed is settled too: the picture does not wait for ever behind a text that will never go.
  Fake second;
  second.targets = {{"bob", ForwardTargetKind::Direct}};
  second.Add(TextRow("t", "first", 1));
  second.HoldFilled(Picture("p", 2, 1));
  second.text_delivery = MessageDelivery::Failed;
  ChatPresenter other;
  std::deque<std::function<void()>> queue;
  other.SetForwardPortsForTest(second.Ports());
  other.SetForwardPostForTest([&queue](std::function<void()> task) { queue.push_back(std::move(task)); });
  ASSERT_TRUE(other.BeginForwardSelection("t"));
  ASSERT_EQ(other.ToggleForwardSelection("p"), ChatPresenter::ForwardTick::Selected);
  other.ForwardSelected({"bob"}, nullptr);
  while (!queue.empty()) {
    auto task = std::move(queue.front());
    queue.pop_front();
    task();
  }
  EXPECT_EQ(second.attachment_calls.size(), 1u);
}

TEST_F(ChatForwardFilesTest, EachTargetIsFilledInOriginalOrderBeforeTheNext) {
  chats.HoldFilled(Picture("p1", 1, 1));
  chats.Add(TextRow("t", "middle", 2));
  chats.HoldFilled(Picture("p2", 3, 2));
  Start({"p1", "t", "p2"}, {"bob", "carol"});
  AnswerUntilDone();
  ASSERT_EQ(done_calls, 1);
  EXPECT_EQ(chats.log, (std::vector<std::string>{"load:1", "att:bob:photo-p1.jpg", "text:bob:middle", "load:2",
                                                 "att:bob:photo-p2.jpg", "load:1", "att:carol:photo-p1.jpg",
                                                 "text:carol:middle", "load:2", "att:carol:photo-p2.jpg"}));
  EXPECT_EQ(result->forwarded, 6u);
}

TEST_F(ChatForwardFilesTest, PlaintextIsLoadedOnlyWhenItsTurnComesAndReleasedAfterTheCallback) {
  chats.HoldFilled(Picture("p1", 1, 1));
  chats.HoldFilled(Picture("p2", 2, 2));
  chats.HoldFilled(Picture("p3", 3, 3));
  Choose({"p1", "p2", "p3"});
  EXPECT_EQ(presenter.CheckForward(1).refusal, ForwardRefusal::None);
  EXPECT_EQ(chats.loads, 0u) << "nothing is read while only choosing";
  presenter.ForwardSelected({"bob"}, [this](const ForwardResult& r) {
    result = r;
    ++done_calls;
  });
  Drain();
  EXPECT_EQ(chats.loads, 1u) << "only the first item's turn has come";
  chats.AnswerOk();
  Drain();
  EXPECT_EQ(chats.loads, 2u);
  chats.AnswerOk();
  Drain();
  EXPECT_EQ(chats.loads, 3u);
  chats.AnswerOk();
  Drain();
  EXPECT_EQ(done_calls, 1);
  EXPECT_EQ(chats.max_unanswered_loads, 1u)
      << "never more than one plaintext out at a time: each one went into its send "
         "(moved, the run keeps no copy) and the next was read after the answer";
}

TEST_F(ChatForwardFilesTest, AnAttachmentWithoutLocalBytesIsCountedFailedAndTheRunGoesOn) {
  chats.Add(Picture("gone", 1, 1)); // no bytes held
  chats.Add(TextRow("t", "text", 2));
  chats.HoldFilled(Picture("here", 3, 2));
  ThreadMessage damaged = Picture("damaged", 4, 3);
  chats.HoldFilled(damaged);
  chats.unreadable.insert(HashOf(3)); // stored, but cannot be decrypted
  Start({"gone", "t", "here", "damaged"}, {"bob"});
  EXPECT_EQ(Attachments("bob"), (std::vector<std::string>{"photo-here.jpg"}));
  chats.AnswerOk();
  Drain();
  Advance(kForwardSettlePollMs);
  ASSERT_EQ(done_calls, 1);
  EXPECT_EQ(result->forwarded, 2u) << "the text and the picture that was here";
  EXPECT_EQ(result->send_errors, 2u) << "the one never downloaded and the one that cannot be read";
  EXPECT_EQ(result->attachments, 3u);
  EXPECT_EQ(result->attachments_failed, 2u);
  EXPECT_FALSE(result->nothing_sent);
  EXPECT_EQ(std::count(chats.log.begin(), chats.log.end(), "load:1"), 0) << "the missing one is never even read";
}

TEST_F(ChatForwardFilesTest, ARateLimitedSendIsRetriedEveryFifteenSecondsUntilItGoesAndIsNotCountedFailed) {
  chats.HoldFilled(Picture("p", 1, 1));
  chats.rate_limited_next = 2;
  Start({"p"}, {"bob"});
  EXPECT_EQ(chats.attachment_calls.size(), 1u) << "refused inside the call";
  EXPECT_TRUE(presenter.ForwardProgressNow().waiting_for_pace);
  EXPECT_FALSE(presenter.ForwardProgressNow().uploading);
  Advance(kForwardRateLimitRetryMs - 1);
  EXPECT_EQ(chats.attachment_calls.size(), 1u) << "not before fifteen seconds";
  Advance(1);
  EXPECT_EQ(chats.attachment_calls.size(), 2u) << "again after fifteen seconds, refused again";
  Advance(kForwardRateLimitRetryMs);
  EXPECT_EQ(chats.attachment_calls.size(), 3u) << "and again, now taken";
  EXPECT_FALSE(presenter.ForwardProgressNow().waiting_for_pace);
  EXPECT_TRUE(presenter.ForwardProgressNow().uploading);
  chats.AnswerOk();
  Drain();
  ASSERT_EQ(done_calls, 1);
  EXPECT_EQ(result->forwarded, 1u);
  EXPECT_EQ(result->send_errors, 0u);
  EXPECT_EQ(result->attachments_failed, 0u) << "a refusal by the send pace is not a failure";
  EXPECT_EQ(chats.loads, 3u) << "each try reads the bytes again: nothing is held while it waits";
}

TEST_F(ChatForwardFilesTest, CancelWorksWhileWaitingForTheRateLimit) {
  chats.HoldFilled(Picture("p", 1, 1));
  chats.rate_limited_next = 5;
  Start({"p"}, {"bob"});
  ASSERT_TRUE(presenter.ForwardProgressNow().waiting_for_pace);
  presenter.CancelForward();
  Drain();
  ASSERT_EQ(done_calls, 1) << "nothing is under way: it ends at once";
  EXPECT_EQ(result->stop, ForwardStop::Cancelled);
  EXPECT_EQ(result->forwarded, 0u);
  EXPECT_EQ(result->send_errors, 0u) << "not counted failed";
  Advance(60000);
  EXPECT_EQ(chats.attachment_calls.size(), 1u) << "the retry timer finds the run gone";
}

TEST_F(ChatForwardFilesTest, NoItemIsEverSentTwiceExceptAfterRateLimited) {
  chats.HoldFilled(Picture("p1", 1, 1));
  chats.HoldFilled(Picture("p2", 2, 2));
  Start({"p1", "p2"}, {"bob"});
  chats.Answer(Error("the upload failed")); // after the messaging layer began: not sent again, whatever it says
  Drain();
  Advance(120000);
  EXPECT_EQ(Attachments("bob"), (std::vector<std::string>{"photo-p1.jpg", "photo-p2.jpg"}))
      << "the failed one is tried once; the next goes on";
  chats.AnswerOk();
  Drain();
  Advance(120000);
  ASSERT_EQ(done_calls, 1);
  EXPECT_EQ(chats.attachment_calls.size(), 2u);
  EXPECT_EQ(result->send_errors, 1u);
  EXPECT_EQ(result->forwarded, 1u);

  // Only a refusal by the send pace is tried again, and then it is the same item: one that went is not.
  Fake pace;
  pace.targets = {{"bob", ForwardTargetKind::Direct}};
  pace.HoldFilled(Picture("q", 1, 1));
  pace.rate_limited_next = 1;
  ChatPresenter other;
  std::deque<std::function<void()>> queue;
  other.SetForwardPortsForTest(pace.Ports());
  other.SetForwardPostForTest([&queue](std::function<void()> task) { queue.push_back(std::move(task)); });
  ASSERT_TRUE(other.BeginForwardSelection("q"));
  other.ForwardSelected({"bob"}, nullptr);
  while (!queue.empty()) {
    auto task = std::move(queue.front());
    queue.pop_front();
    task();
  }
  ASSERT_EQ(pace.timers.size(), 1u);
  pace.now += kForwardRateLimitRetryMs;
  pace.timers.front().task();
  while (!queue.empty()) {
    auto task = std::move(queue.front());
    queue.pop_front();
    task();
  }
  EXPECT_EQ(pace.attachment_calls.size(), 2u) << "one refused, one taken";
  pace.AnswerOk();
  while (!queue.empty()) {
    auto task = std::move(queue.front());
    queue.pop_front();
    task();
  }
  EXPECT_EQ(pace.attachment_calls.size(), 2u) << "never a third";
  EXPECT_TRUE(pace.in_flight.empty());
}

TEST_F(ChatForwardFilesTest, MoreThanFiveUploadsIsRefusedBeforeAnythingStarts) {
  for (int i = 1; i <= 3; ++i) {
    chats.HoldFilled(Picture("p" + std::to_string(i), i, static_cast<uint8_t>(i)));
  }
  Choose({"p1", "p2", "p3"});
  EXPECT_EQ(presenter.CheckForward(1).refusal, ForwardRefusal::None) << "3 uploads";
  const ForwardCheck six = presenter.CheckForward(2);
  EXPECT_EQ(six.refusal, ForwardRefusal::TooManyUploads) << "3 attachments x 2 chats";
  EXPECT_EQ(six.uploads, 6u);

  presenter.ForwardSelected({"bob", "carol"}, [this](const ForwardResult& r) {
    result = r;
    ++done_calls;
  });
  ASSERT_EQ(done_calls, 1) << "told at once";
  EXPECT_EQ(result->refusal, ForwardRefusal::TooManyUploads);
  EXPECT_TRUE(result->nothing_sent);
  EXPECT_TRUE(chats.log.empty()) << "nothing was read or sent";
  EXPECT_EQ(chats.loads, 0u);
  EXPECT_TRUE(presenter.ForwardChoosing()) << "the choosing stays: take some away and send again";
  EXPECT_FALSE(presenter.Forwarding());

  presenter.ForwardSelected({"bob"}, [this](const ForwardResult& r) { result = r; });
  EXPECT_FALSE(presenter.ForwardChoosing()) << "three uploads are fine";
  EXPECT_TRUE(presenter.Forwarding());
}

TEST_F(ChatForwardFilesTest, MoreThanTwentyFiveMegabytesIsRefusedBeforeAnythingStarts) {
  chats.HoldFilled(Picture("a", 1, 1, 10 * kMb));
  chats.HoldFilled(Picture("b", 2, 2, 10 * kMb));
  chats.HoldFilled(Picture("c", 3, 3, 10 * kMb));
  Choose({"a", "b", "c"});
  const ForwardCheck check = presenter.CheckForward(1);
  EXPECT_EQ(check.refusal, ForwardRefusal::TooLarge);
  EXPECT_EQ(check.upload_bytes, 30 * kMb);
  presenter.ForwardSelected({"bob"}, [this](const ForwardResult& r) {
    result = r;
    ++done_calls;
  });
  ASSERT_EQ(done_calls, 1);
  EXPECT_EQ(result->refusal, ForwardRefusal::TooLarge);
  EXPECT_TRUE(chats.log.empty());
  EXPECT_EQ(chats.loads, 0u);
  presenter.EndForwardSelection();

  // Each chat takes its own upload: 2 x 10 MB to two chats is 40 MB.
  Choose({"a", "b"});
  EXPECT_EQ(presenter.CheckForward(1).refusal, ForwardRefusal::None) << "20 MB";
  EXPECT_EQ(presenter.CheckForward(2).refusal, ForwardRefusal::TooLarge) << "40 MB";
  presenter.EndForwardSelection();
  // The limit itself is allowed.
  chats.HoldFilled(Picture("e", 4, 4, kMaxForwardUploadBytes / 2));
  chats.HoldFilled(Picture("f", 5, 5, kMaxForwardUploadBytes / 2));
  Choose({"e", "f"});
  EXPECT_EQ(presenter.CheckForward(1).refusal, ForwardRefusal::None) << "exactly 25 MB";
  presenter.EndForwardSelection();
  chats.HoldFilled(Picture("g", 6, 6, kMaxForwardUploadBytes / 2 + 1));
  Choose({"e", "g"});
  EXPECT_EQ(presenter.CheckForward(1).refusal, ForwardRefusal::TooLarge) << "a byte over";
}

TEST_F(ChatForwardFilesTest, TextAndCardsDoNotCountTowardsTheLimits) {
  chats.HoldFilled(Picture("p", 1, 1, 8 * kMb));
  std::vector<std::string> ids = {"p"};
  for (int i = 0; i < 20; ++i) {
    chats.Add(TextRow("t" + std::to_string(i), std::string(2000, 'x'), 10 + i));
    ids.push_back("t" + std::to_string(i));
  }
  chats.Add(CardRow("card", 40));
  ids.push_back("card");
  Choose(ids);
  const ForwardCheck check = presenter.CheckForward(3);
  EXPECT_EQ(check.refusal, ForwardRefusal::None);
  EXPECT_EQ(check.uploads, 3u) << "only the picture, once per chat";
  EXPECT_EQ(check.upload_bytes, 24 * kMb);
  EXPECT_EQ(check.attachments, 1u);
  presenter.EndForwardSelection();

  Choose({"t0", "t1", "card"});
  const ForwardCheck texts_only = presenter.CheckForward(9);
  EXPECT_EQ(texts_only.refusal, ForwardRefusal::None);
  EXPECT_EQ(texts_only.uploads, 0u);
  EXPECT_EQ(texts_only.upload_bytes, 0u);
  presenter.SetForwardAttachmentBusy([]() { return true; });
  EXPECT_EQ(presenter.CheckForward(9).refusal, ForwardRefusal::None) << "text and cards go while an attachment is busy";
}

TEST_F(ChatForwardFilesTest, ATargetThatCanNoLongerBeWrittenToFailsItsRemainingItems) {
  chats.HoldFilled(Picture("p1", 1, 1));
  chats.HoldFilled(Picture("p2", 2, 2));
  chats.Add(TextRow("t", "text", 3));
  Start({"p1", "p2", "t"}, {"bob", "carol"});
  chats.targets["bob"] = ForwardTargetKind::Unavailable; // blocked, or removed from the group, during the first upload
  chats.AnswerOk();
  Drain();
  EXPECT_EQ(Attachments("bob"), (std::vector<std::string>{"photo-p1.jpg"})) << "nothing more to bob";
  AnswerUntilDone();
  ASSERT_EQ(done_calls, 1);
  EXPECT_EQ(Attachments("carol"), (std::vector<std::string>{"photo-p1.jpg", "photo-p2.jpg"}));
  EXPECT_EQ(chats.log.back(), "text:carol:text");
  EXPECT_EQ(result->skipped_targets, 1u);
  EXPECT_EQ(result->send_errors, 2u) << "the second picture and the text did not reach bob";
  EXPECT_EQ(result->forwarded, 4u) << "one to bob, three to carol";
}

TEST_F(ChatForwardFilesTest, CancelStopsTheRestAndKeepsWhatWasSent) {
  for (int i = 1; i <= 3; ++i) {
    chats.HoldFilled(Picture("p" + std::to_string(i), i, static_cast<uint8_t>(i)));
  }
  Start({"p1", "p2", "p3"}, {"bob"});
  chats.AnswerOk();
  Drain(); // p2 is under way
  presenter.CancelForward();
  Drain();
  chats.AnswerOk();
  Drain();
  Advance(120000);
  ASSERT_EQ(done_calls, 1);
  EXPECT_EQ(Attachments("bob"), (std::vector<std::string>{"photo-p1.jpg", "photo-p2.jpg"}))
      << "the third never started";
  EXPECT_EQ(result->forwarded, 2u) << "what was sent stays sent";
  EXPECT_EQ(result->stop, ForwardStop::Cancelled);
  EXPECT_FALSE(result->nothing_sent);
  EXPECT_FALSE(presenter.TakeForwardNotice().has_value()) << "the result told it; no second notice";
}

TEST_F(ChatForwardFilesTest, BackgroundingStopsTheRunAndMarksPartialForThatAccount) {
  chats.HoldFilled(Picture("p1", 1, 1));
  chats.HoldFilled(Picture("p2", 2, 2));
  Start({"p1", "p2"}, {"bob", "carol"});
  chats.AnswerOk();
  Drain(); // p2 to bob is under way (2 of 4 are left after it)
  presenter.NoteAppEnteredBackground();
  EXPECT_FALSE(presenter.Forwarding()) << "the page closes at once";
  EXPECT_EQ(done_calls, 0) << "nobody is told by a page that is gone";
  chats.AnswerOk(); // the item under way is not interrupted: it was handed over
  Drain();
  Advance(120000);
  EXPECT_EQ(chats.attachment_calls.size(), 2u) << "nothing else was started";

  chats.account = "acct-b";
  EXPECT_FALSE(presenter.TakeForwardNotice().has_value()) << "another account never hears of it";
  chats.account = "acct-a";
  const auto notice = presenter.TakeForwardNotice();
  ASSERT_TRUE(notice.has_value());
  EXPECT_EQ(notice->kind, ForwardStop::Interrupted);
  EXPECT_EQ(notice->done, 2u);
  EXPECT_EQ(notice->total, 4u);
  EXPECT_FALSE(presenter.TakeForwardNotice().has_value()) << "once";
}

TEST_F(ChatForwardFilesTest, LosingFocusOnlyDoesNotStopTheRun) {
  chats.HoldFilled(Picture("p1", 1, 1));
  chats.HoldFilled(Picture("p2", 2, 2));
  Start({"p1", "p2"}, {"bob"});
  presenter.NoteAppLostFocus(); // Control Center, a banner: iOS willResignActive
  EXPECT_TRUE(presenter.Forwarding());
  chats.AnswerOk();
  Drain();
  EXPECT_EQ(chats.attachment_calls.size(), 2u) << "it goes on";
  chats.AnswerOk();
  Drain();
  Advance(kForwardSettlePollMs);
  ASSERT_EQ(done_calls, 1);
  EXPECT_EQ(result->forwarded, 2u);
  EXPECT_FALSE(presenter.TakeForwardNotice().has_value());
}

TEST_F(ChatForwardFilesTest, LockStopsTheRun) {
  chats.HoldFilled(Picture("p1", 1, 1));
  chats.HoldFilled(Picture("p2", 2, 2));
  Start({"p1", "p2"}, {"bob"});
  chats.AnswerOk();
  Drain();
  presenter.AbortForward(); // the vault locks
  EXPECT_FALSE(presenter.Forwarding());
  chats.ready = false;
  chats.AnswerOk();
  Drain();
  Advance(120000);
  EXPECT_EQ(chats.attachment_calls.size(), 2u);
  EXPECT_EQ(done_calls, 0);
  const auto notice = presenter.TakeForwardNotice();
  ASSERT_TRUE(notice.has_value());
  EXPECT_EQ(notice->done, 2u);
  EXPECT_EQ(notice->total, 2u);
}

TEST_F(ChatForwardFilesTest, SignOutStopsTheRun) {
  chats.HoldFilled(Picture("p1", 1, 1));
  chats.HoldFilled(Picture("p2", 2, 2));
  Start({"p1", "p2"}, {"bob"});
  chats.AnswerOk();
  Drain();
  presenter.ResetForNewProfile();
  EXPECT_FALSE(presenter.Forwarding());
  EXPECT_TRUE(presenter.ForwardNoticePending()) << "the second picture is still under way";
  chats.AnswerOk();
  Drain();
  Advance(120000);
  EXPECT_EQ(chats.attachment_calls.size(), 2u) << "nothing started after it";
  EXPECT_EQ(done_calls, 0);
  const auto notice = presenter.TakeForwardNotice();
  ASSERT_TRUE(notice.has_value());
  EXPECT_EQ(notice->done, 2u);
}

TEST_F(ChatForwardFilesTest, CancelWaitsForTheItemInFlightThenReportsDoneOfTotal) {
  for (int i = 1; i <= 3; ++i) {
    chats.HoldFilled(Picture("p" + std::to_string(i), i, static_cast<uint8_t>(i)));
  }
  Start({"p1", "p2", "p3"}, {"bob"});
  chats.AnswerOk();
  Drain(); // 1 of 3 done, the second under way
  presenter.CancelForward();
  Drain();
  EXPECT_EQ(done_calls, 0) << "it waits for the one under way";
  EXPECT_TRUE(presenter.Forwarding());
  const ForwardProgress progress = presenter.ForwardProgressNow();
  EXPECT_TRUE(progress.finishing);
  EXPECT_FALSE(progress.can_close);
  EXPECT_EQ(progress.done, 1u);
  EXPECT_EQ(progress.total, 3u);
  chats.AnswerOk();
  Drain();
  ASSERT_EQ(done_calls, 1);
  EXPECT_EQ(result->forwarded, 2u) << "the one that was under way counts";
  EXPECT_EQ(result->total_items, 3u);
  EXPECT_EQ(result->stop, ForwardStop::Cancelled);
  EXPECT_EQ(chats.attachment_calls.size(), 2u);
}

TEST_F(ChatForwardFilesTest, ACallbackAfterBackgroundStopUpdatesTheDoneCountAndShowsNothingElse) {
  chats.HoldFilled(Picture("p1", 1, 1));
  chats.HoldFilled(Picture("p2", 2, 2));
  chats.Add(TextRow("t", "text", 3));
  Start({"p1", "p2", "t"}, {"bob"});
  presenter.NoteAppEnteredBackground(); // the first is under way and nothing is done
  EXPECT_FALSE(presenter.TakeForwardNotice().has_value());
  chats.AnswerOk();
  Drain();
  Advance(120000);
  EXPECT_EQ(done_calls, 0) << "no result, no page";
  EXPECT_TRUE(sink.empty()) << "no pill of its own";
  EXPECT_EQ(chats.attachment_calls.size(), 1u) << "the next picture is not started";
  EXPECT_EQ(std::count(chats.log.begin(), chats.log.end(), "text:bob:text"), 0) << "nor the text";
  const auto notice = presenter.TakeForwardNotice();
  ASSERT_TRUE(notice.has_value()) << "the picture that went out is a part";
  EXPECT_EQ(notice->done, 1u);
  EXPECT_EQ(notice->total, 3u);
}

TEST_F(ChatForwardFilesTest, DeletingTheSourceChatStopsTheRunAndMarksPartial) {
  chats.HoldFilled(Picture("p1", 1, 1));
  chats.HoldFilled(Picture("p2", 2, 2));
  Start({"p1", "p2"}, {"bob"});
  chats.AnswerOk();
  Drain();
  presenter.ForgetThread("another-chat");
  EXPECT_TRUE(presenter.Forwarding()) << "another chat's deletion changes nothing";
  presenter.ForgetThread("src"); // the chat the messages came from is gone
  EXPECT_FALSE(presenter.Forwarding());
  ASSERT_EQ(sink.size(), 1u);
  EXPECT_EQ(sink[0].kind, ForwardStop::SourceGone);
  EXPECT_EQ(sink[0].done, 1u);
  EXPECT_EQ(sink[0].total, 2u);
  chats.AnswerOk();
  Drain();
  Advance(120000);
  EXPECT_EQ(chats.attachment_calls.size(), 2u);
  EXPECT_EQ(done_calls, 0);
}

TEST_F(ChatForwardFilesTest, OnDesktopLosingFocusOrMinimisingDoesNotStopTheRun) {
  // A desktop never gets the phone's "really in the background" (PlatformLifecycleEvents): its focus and minimise
  // notices are the presenter's no-ops, and only lock, sign-out and quitting stop it.
  chats.HoldFilled(Picture("p1", 1, 1));
  chats.HoldFilled(Picture("p2", 2, 2));
  Start({"p1", "p2"}, {"bob"});
  presenter.NoteAppLostFocus();
  presenter.NoteAppMinimized();
  Advance(120000);
  EXPECT_TRUE(presenter.Forwarding());
  chats.AnswerOk();
  Drain();
  presenter.NoteAppLostFocus();
  presenter.NoteAppMinimized();
  EXPECT_EQ(chats.attachment_calls.size(), 2u);
  chats.AnswerOk();
  Drain();
  Advance(kForwardSettlePollMs);
  ASSERT_EQ(done_calls, 1);
  EXPECT_EQ(result->forwarded, 2u);
  EXPECT_EQ(result->stop, ForwardStop::None);
}

TEST_F(ChatForwardFilesTest, ItemsWithoutLocalBytesDoNotCountTowardsTheLimits) {
  chats.HoldFilled(Picture("held", 1, 1, 4 * kMb));
  for (int i = 2; i <= 7; ++i) {
    chats.Add(Picture("far" + std::to_string(i), i, static_cast<uint8_t>(i), 40 * kMb)); // bytes not held
  }
  Choose({"held", "far2", "far3", "far4", "far5", "far6", "far7"});
  const ForwardCheck check = presenter.CheckForward(1);
  EXPECT_EQ(check.refusal, ForwardRefusal::None) << "six far ones over the count and over the size, but not held";
  EXPECT_EQ(check.attachments, 7u);
  EXPECT_EQ(check.without_local_bytes, 6u);
  EXPECT_EQ(check.uploads, 1u);
  EXPECT_EQ(check.upload_bytes, 4 * kMb);
  ASSERT_EQ(check.items.size(), 7u);
  EXPECT_TRUE(check.items[0].has_local_bytes);
  EXPECT_FALSE(check.items[1].has_local_bytes) << "the preview greys it";
}

TEST_F(ChatForwardFilesTest, HavingLocalBytesIsDecidedWithoutDecrypting) {
  chats.HoldFilled(Picture("held", 1, 1));
  chats.Add(Picture("far", 2, 2));
  chats.Add(TextRow("t", "text", 3));
  chats.targets["bob"] = ForwardTargetKind::Direct;
  EXPECT_TRUE(presenter.CanForward("held"));
  Choose({"held", "far", "t"});
  const ForwardCheck check = presenter.CheckForward(2);
  EXPECT_EQ(check.without_local_bytes, 1u);
  (void)presenter.ForwardPreview([](size_t count) { return std::to_string(count); });
  (void)presenter.ForwardTargets();
  EXPECT_GT(chats.stats, 0u) << "it asked the store whether the bytes are there";
  EXPECT_EQ(chats.loads, 0u) << "and never asked for the plaintext";
}

TEST_F(ChatForwardFilesTest, ASingleAttachmentOverTheSizeLimitGetsItsOwnMessage) {
  chats.HoldFilled(Picture("big", 1, 1, 30 * kMb));
  Choose({"big"});
  const ForwardCheck alone = presenter.CheckForward(1);
  EXPECT_EQ(alone.refusal, ForwardRefusal::SingleTooLarge) << "not the 'split it over several' message";
  EXPECT_EQ(alone.largest_bytes, 30 * kMb);
  presenter.EndForwardSelection();

  chats.HoldFilled(Picture("a", 2, 2, 20 * kMb));
  chats.HoldFilled(Picture("b", 3, 3, 20 * kMb));
  Choose({"a", "b"});
  EXPECT_EQ(presenter.CheckForward(1).refusal, ForwardRefusal::TooLarge) << "each fits; together they do not";
  presenter.EndForwardSelection();
  Choose({"big", "a", "b"});
  EXPECT_EQ(presenter.CheckForward(1).refusal, ForwardRefusal::SingleTooLarge) << "the one that can never go is named";
  presenter.ForwardSelected({"bob"}, [this](const ForwardResult& r) {
    result = r;
    ++done_calls;
  });
  ASSERT_EQ(done_calls, 1);
  EXPECT_EQ(result->refusal, ForwardRefusal::SingleTooLarge);
  EXPECT_TRUE(chats.log.empty());
}

TEST_F(ChatForwardFilesTest, CancelThatGetsNoCallbackOffersCloseAfterFifteenSecondsAndCountsNothingAsFailed) {
  chats.HoldFilled(Picture("p1", 1, 1));
  chats.HoldFilled(Picture("p2", 2, 2));
  Start({"p1", "p2"}, {"bob"});
  presenter.CancelForward();
  Drain();
  EXPECT_TRUE(presenter.ForwardProgressNow().finishing);
  Advance(kForwardCloseOfferMs - 1);
  EXPECT_FALSE(presenter.ForwardProgressNow().can_close);
  presenter.CloseForward();
  EXPECT_TRUE(presenter.Forwarding()) << "too early: Close is not offered yet";
  Advance(1);
  EXPECT_TRUE(presenter.ForwardProgressNow().can_close);
  presenter.CloseForward();
  EXPECT_FALSE(presenter.Forwarding());
  ASSERT_EQ(done_calls, 1) << "the page is told, with what went out";
  EXPECT_EQ(result->stop, ForwardStop::Abandoned);
  EXPECT_EQ(result->forwarded, 0u);
  EXPECT_EQ(result->send_errors, 0u) << "the item under way is not counted failed";
  EXPECT_EQ(result->attachments_failed, 0u);
  EXPECT_EQ(result->total_items, 2u);
  chats.AnswerOk(); // it answers much later: nothing visible changes
  Drain();
  Advance(120000);
  EXPECT_EQ(done_calls, 1);
  EXPECT_EQ(chats.attachment_calls.size(), 1u);
}

TEST_F(ChatForwardFilesTest, TheNoticeAfterBackgroundWaitsForTheItemInFlight) {
  chats.HoldFilled(Picture("p1", 1, 1));
  chats.HoldFilled(Picture("p2", 2, 2));
  chats.Add(TextRow("t", "text", 0));
  chats.targets["bob"] = ForwardTargetKind::Direct;
  Start({"t", "p1", "p2"}, {"bob"}); // the text went; p1 is under way
  presenter.NoteAppEnteredBackground();
  EXPECT_TRUE(presenter.ForwardNoticePending()) << "'one is still being sent...'";
  EXPECT_FALSE(presenter.TakeForwardNotice().has_value()) << "not before it has an answer";
  EXPECT_TRUE(presenter.ForwardNoticePending());
  chats.AnswerOk(); // back in front, the system lets the upload finish
  Drain();
  EXPECT_FALSE(presenter.ForwardNoticePending());
  const auto notice = presenter.TakeForwardNotice();
  ASSERT_TRUE(notice.has_value());
  EXPECT_EQ(notice->done, 2u) << "the text and the picture that was under way";
  EXPECT_EQ(notice->total, 3u);

  // An answer that never comes does not keep the notice back for ever.
  Fake second;
  second.targets = {{"bob", ForwardTargetKind::Direct}};
  second.HoldFilled(Picture("q1", 1, 1));
  second.Add(TextRow("tt", "text", 0));
  ChatPresenter other;
  std::deque<std::function<void()>> queue;
  other.SetForwardPortsForTest(second.Ports());
  other.SetForwardPostForTest([&queue](std::function<void()> task) { queue.push_back(std::move(task)); });
  ASSERT_TRUE(other.BeginForwardSelection("tt"));
  ASSERT_EQ(other.ToggleForwardSelection("q1"), ChatPresenter::ForwardTick::Selected);
  other.ForwardSelected({"bob"}, nullptr);
  while (!queue.empty()) {
    auto task = std::move(queue.front());
    queue.pop_front();
    task();
  }
  other.NoteAppEnteredBackground();
  EXPECT_FALSE(other.TakeForwardNotice().has_value());
  second.now += kForwardNoticeWaitMs;
  const auto late = other.TakeForwardNotice();
  ASSERT_TRUE(late.has_value());
  EXPECT_EQ(late->done, 1u) << "the text; the picture that never answered is not counted";
}

TEST_F(ChatForwardFilesTest, ASourceChatDeletedElsewhereShowsThePillAtOnce) {
  chats.HoldFilled(Picture("p1", 1, 1));
  chats.HoldFilled(Picture("p2", 2, 2));
  Start({"p1", "p2"}, {"bob"});
  EXPECT_TRUE(sink.empty());
  presenter.NoteForwardSourceGone("src");
  ASSERT_EQ(sink.size(), 1u) << "told now, while the item is still under way";
  EXPECT_EQ(sink[0].kind, ForwardStop::SourceGone);
  EXPECT_EQ(sink[0].done, 0u);
  EXPECT_EQ(sink[0].total, 2u);
  EXPECT_FALSE(presenter.Forwarding());
  EXPECT_FALSE(presenter.TakeForwardNotice().has_value()) << "not again on the next chat entry";
  chats.AnswerOk();
  Drain();
  EXPECT_EQ(sink.size(), 1u);
  EXPECT_EQ(done_calls, 0);
}

TEST_F(ChatForwardFilesTest, ANotificationTapStopsTheRunAndSaysItWasStopped) {
  chats.HoldFilled(Picture("p1", 1, 1));
  chats.HoldFilled(Picture("p2", 2, 2));
  Start({"p1", "p2"}, {"bob"});
  presenter.CancelForward(ForwardStop::Tapped); // the tap closed the page: the same as Cancel, said differently
  Drain();
  EXPECT_EQ(done_calls, 0) << "the item under way is waited for";
  chats.AnswerOk();
  Drain();
  Advance(120000);
  ASSERT_EQ(done_calls, 1);
  EXPECT_EQ(result->stop, ForwardStop::Tapped) << "not Cancelled: the user did not press Cancel";
  EXPECT_EQ(result->forwarded, 1u);
  EXPECT_EQ(result->total_items, 2u);
  EXPECT_EQ(chats.attachment_calls.size(), 1u);
}

TEST_F(ChatForwardFilesTest, AnItemWhoseRealSizeBreaksTheLimitIsCountedFailedAndNotSent) {
  // The sizes in the messages are the sender's word: these say 1 MB, the bytes held are what counts.
  chats.Hold(Picture("p1", 1, 1, kMb), ByteVector(20 * kMb, 1));
  chats.Hold(Picture("p2", 2, 2, kMb), ByteVector(10 * kMb, 2));
  chats.Hold(Picture("p3", 3, 3, kMb), ByteVector(26 * kMb, 3));
  chats.HoldFilled(Picture("p4", 4, 4, kMb), 1000);
  Choose({"p1", "p2", "p3", "p4"});
  EXPECT_EQ(presenter.CheckForward(1).refusal, ForwardRefusal::None) << "the messages claim 4 MB";
  presenter.ForwardSelected({"bob"}, [this](const ForwardResult& r) {
    result = r;
    ++done_calls;
  });
  Drain();
  chats.AnswerOk(); // p1: 20 MB went
  Drain();
  EXPECT_EQ(Attachments("bob"), (std::vector<std::string>{"photo-p1.jpg", "photo-p4.jpg"}))
      << "p2 would take it over 25 MB, p3 alone is over 25 MB: neither is sent; p4 still fits";
  chats.AnswerOk();
  Drain();
  Advance(kForwardSettlePollMs);
  ASSERT_EQ(done_calls, 1);
  EXPECT_EQ(result->forwarded, 2u);
  EXPECT_EQ(result->send_errors, 2u);
  EXPECT_EQ(result->attachments_failed, 2u);
}

TEST_F(ChatForwardFilesTest, AVoiceMessageIsResentWithItsOriginalMimeAndFilename) {
  const ThreadMessage voice = AttachmentRow("v", 1, "voice-4200.ogg", "audio/ogg", 20000, 7);
  chats.Hold(voice, ByteVector{1, 2, 3, 4, 5, 6, 7, 8});
  EXPECT_EQ(ForwardKindOf(voice), ForwardItemKind::Voice);
  Start({"v"}, {"bob", "club"});
  ASSERT_EQ(chats.attachment_calls.size(), 1u);
  EXPECT_EQ(chats.attachment_calls[0].attachment.mime, "audio/ogg");
  EXPECT_EQ(chats.attachment_calls[0].attachment.filename, "voice-4200.ogg") << "the length rides in the name";
  EXPECT_EQ(chats.attachment_calls[0].attachment.bytes, (ByteVector{1, 2, 3, 4, 5, 6, 7, 8}));
  AnswerUntilDone();
  ASSERT_EQ(done_calls, 1);
  ASSERT_EQ(chats.attachment_calls.size(), 2u);
  EXPECT_EQ(chats.attachment_calls[1].thread, "club") << "a group takes it the same way";
  EXPECT_EQ(chats.attachment_calls[1].attachment.filename, "voice-4200.ogg");
}

TEST_F(ChatForwardFilesTest, APictureIsResentAsNotShrunk) {
  const ThreadMessage picture = Picture("p", 1, 1, 6 * kMb);
  chats.Hold(picture, ByteVector(6 * kMb, 1));
  EXPECT_EQ(ForwardKindOf(picture), ForwardItemKind::Picture);
  chats.Hold(AttachmentRow("m", 2, "clip.mp4", "video/mp4", 3 * kMb, 2), ByteVector(3 * kMb, 2));
  chats.HoldFilled(AttachmentRow("f", 3, "notes.pdf", "application/pdf", 90000, 3));
  EXPECT_EQ(ForwardKindOf(*chats.Ports().get_message("src", "m")), ForwardItemKind::Video);
  EXPECT_EQ(ForwardKindOf(*chats.Ports().get_message("src", "f")), ForwardItemKind::File);
  Start({"p", "m", "f"}, {"bob"});
  AnswerUntilDone();
  ASSERT_EQ(done_calls, 1);
  ASSERT_EQ(chats.attachment_calls.size(), 3u);
  for (const AttachmentCall& call : chats.attachment_calls) {
    EXPECT_FALSE(call.attachment.image_is_shrunk) << call.attachment.filename << ": final bytes, no shrink";
  }
  EXPECT_EQ(chats.attachment_calls[0].attachment.mime, "image/jpeg");
}

TEST_F(ChatForwardFilesTest, TheForwardSendsOnlyBytesMimeAndNameNeverTheOriginalUrlOrKey) {
  const ThreadMessage picture = Picture("p", 1, 9, 50);
  chats.Hold(picture, ByteVector(50, 0x42));
  Start({"p"}, {"bob"});
  ASSERT_EQ(chats.attachment_calls.size(), 1u);
  const OutgoingAttachment& sent = chats.attachment_calls[0].attachment;
  EXPECT_EQ(sent.bytes, ByteVector(50, 0x42)) << "the plaintext as stored";
  EXPECT_EQ(sent.mime, "image/jpeg");
  EXPECT_EQ(sent.filename, "photo-p.jpg");
  // OutgoingAttachment has no url, key or nonce field: whatever it is handed, the original's cannot ride along. A
  // fresh upload makes its own, so the receiver cannot tell it from a picture we sent ourselves.
  EXPECT_EQ(sent.filename.find("secret-url"), std::string::npos);
  EXPECT_NE(sent.bytes, ByteVector(32, 0xEE)) << "the key is not the bytes";
  chats.AnswerOk();
  Drain();
  ASSERT_EQ(chats.log.size(), 2u);
  EXPECT_EQ(chats.log[1], "att:bob:photo-p.jpg");
}

TEST_F(ChatForwardFilesTest, FailedCountIsPerMessageNotPerUpload) {
  chats.Add(Picture("far", 1, 1)); // not held
  chats.HoldFilled(Picture("held", 2, 2));
  Start({"far", "held"}, {"bob", "carol", "club"});
  AnswerUntilDone();
  ASSERT_EQ(done_calls, 1);
  EXPECT_EQ(result->send_errors, 1u) << "one message did not go, to three chats";
  EXPECT_EQ(result->attachments, 6u) << "six uploads were asked for";
  EXPECT_EQ(result->attachments_failed, 3u) << "and three of them did not happen";
  EXPECT_EQ(result->forwarded, 3u);
  EXPECT_EQ(result->total_items, 6u);
}

TEST_F(ChatForwardFilesTest, TheMetricCountsAttachmentsAndFailuresWithoutIds) {
  auto capture = std::make_shared<MetricsCapture>();
  logging::getLogger("Metrics").addHandler(capture);
  chats.active = "secret-source-chat";
  chats.targets = {{"secret-target-1", ForwardTargetKind::Direct}, {"secret-target-2", ForwardTargetKind::Group}};
  ThreadMessage held = Picture("secret-held", 1, 1);
  held.thread_id = "secret-source-chat";
  chats.HoldFilled(held);
  chats.Add(Picture("secret-far", 2, 2)); // not held: fails in both chats
  chats.Add(TextRow("secret-text", "words nobody may log", 3));
  Start({"secret-held", "secret-far", "secret-text"}, {"secret-target-1", "secret-target-2"});
  AnswerUntilDone();
  ASSERT_EQ(done_calls, 1);

  std::string line;
  for (const std::string& candidate : capture->Lines()) {
    if (candidate.find("event=chat.forward") != std::string::npos) {
      EXPECT_TRUE(line.empty()) << "one line per forward";
      line = candidate;
    }
  }
  ASSERT_FALSE(line.empty());
  EXPECT_NE(line.find("attachments=4"), std::string::npos) << line;
  EXPECT_NE(line.find("attachments_failed=2"), std::string::npos) << line;
  EXPECT_NE(line.find("messages=3"), std::string::npos) << line;
  EXPECT_EQ(line.find("secret"), std::string::npos) << "no chat or message id, no name, no words: " << line;
  EXPECT_EQ(line.find("photo"), std::string::npos) << "no file name: " << line;
}

} // namespace
} // namespace pbr
