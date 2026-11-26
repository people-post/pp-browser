// Forwarding pictures, videos, files and voice messages, the phone screens' side (DEV/PLAN-message-forward-files-v3.md,
// step 3): the picker's preview and the reasons Send is off, the "Forwarding 2/7" page and everything that opens, draws
// and closes it, the screen held on by it, and a call that must never sit hidden under it. The real ChatPresenter and
// ForwardFlow run against fakes of the messaging calls and of the screen; nothing sleeps.
#include "gui/chat/ForwardFlow.h"
#include "gui/chat/ForwardPreview.h"
#include "gui/chat/ForwardProgressController.h"
#include "gui/ScreenAwakeHolds.h"

#include "feature/presenters/ChatPresenter.h"
#include "foundation/i18n/LocalizationService.h"
#include "lib/delivery/ChatPayloadCodec.h"
#include "lib/voice/VoiceFile.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <deque>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

namespace pbr {
namespace {

constexpr uint64_t kMb = 1000ULL * 1000ULL;

std::string ReadAll(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream out;
  out << in.rdbuf();
  return out.str();
}

std::vector<uint8_t> HashOf(const uint8_t tag) {
  return std::vector<uint8_t>(32, tag);
}

ThreadMessage Attachment(const std::string& id, const int64_t order, const std::string& filename,
                         const std::string& mime, const uint64_t declared, const uint8_t tag) {
  ChatAttachmentFields fields;
  fields.url = "https://relay.example/" + id;
  fields.mime = mime;
  fields.filename = filename;
  fields.byte_length = declared;
  fields.content_hash = HashOf(tag);
  fields.content_key = std::vector<uint8_t>(32, 0xEE);
  fields.blob_nonce = std::vector<uint8_t>(24, 0xDD);
  ThreadMessage row;
  row.id = id;
  row.thread_id = "src";
  row.sender_contact_id = "contact-a";
  row.content_type = ChatContentType::Attachment;
  row.text = filename;
  row.payload_json = ChatPayloadCodec::AttachmentFieldsToJson(fields);
  row.display_order = order;
  row.timestamp = 1000 + order;
  row.delivery = MessageDelivery::Relayed;
  return row;
}

ThreadMessage Picture(const std::string& id, const int64_t order, const uint8_t tag, const uint64_t declared = 1000) {
  return Attachment(id, order, "photo-" + id + ".jpg", "image/jpeg", declared, tag);
}

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

struct World {
  struct Upload {
    std::string thread;
    std::function<void(Roe<ThreadMessage>)> on_done;
    std::function<bool(uint64_t, uint64_t)> progress;
  };

  // The flow's destructor lets the screen go (keep_awake(false) writes into `awake`, a member declared after it): the
  // flow goes first, while every member it calls into is still alive. Without this a run still going at the end of a
  // test wrote into a freed vector and corrupted the heap, and a later test crashed (about every other run).
  ~World() { flow.reset(); }

  World() {
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
      log.push_back("text:" + thread + ":" + text);
      ThreadMessage sent;
      sent.id = "sent-" + std::to_string(++sent_ids);
      sent.delivery = MessageDelivery::Relayed;
      rows[sent.id] = sent;
      return Roe<ThreadMessage>(sent);
    };
    ports.send_group = ports.send_user;
    ports.local_bytes = [this](const std::vector<uint8_t>& hash) -> uint64_t {
      ++stats;
      const auto it = store.find(hash);
      return it == store.end() ? 0 : it->second.size() + 16;
    };
    ports.load_plaintext = [this](const std::vector<uint8_t>& hash) -> Roe<ByteVector> {
      ++loads;
      const auto it = store.find(hash);
      if (it == store.end()) {
        return Error("not here");
      }
      return it->second;
    };
    ports.send_attachment = [this](const std::string& thread, OutgoingAttachment attachment,
                                   std::function<void(Roe<ThreadMessage>)> on_done,
                                   std::function<bool(uint64_t, uint64_t)> progress) {
      log.push_back("att:" + thread + ":" + attachment.filename);
      uploads.push_back({thread, std::move(on_done), std::move(progress)});
    };
    ports.now_ms = [this]() { return now; };
    ports.after = [this](const int64_t delay_ms, std::function<void()> task) {
      timers.push_back({now + delay_ms, std::move(task)});
    };
    presenter.SetForwardPortsForTest(ports);
    presenter.SetForwardPostForTest([this](std::function<void()> task) { posted.push_back(std::move(task)); });
    presenter.SetForwardAttachmentBusy([this]() { return attachment_busy; });
    presenter.SetForwardCallActive([this]() { return call_active; });

    ForwardFlowPorts screen;
    screen.open_picker = [this](PeoplePickerChooseOptions options, PeoplePickerChooseDone done,
                                std::function<void()> cancel) {
      picker_open = true;
      ++pickers_opened;
      picker_options = std::move(options);
      picker_done = std::move(done);
      picker_cancel = std::move(cancel);
    };
    screen.close_picker = [this]() { picker_open = false; };
    screen.pill = [this](const std::string& message) { pills.push_back(message); };
    screen.changed = []() {};
    screen.open_progress = [this]() {
      ++progress_opened;
      progress_open = true;
    };
    screen.close_progress = [this]() {
      ++progress_closed;
      progress_open = false;
    };
    screen.show_progress = [this](const ForwardProgressView& view) { views.push_back(view); };
    screen.keep_awake = [this](const bool on) { awake.push_back(on); };
    screen.now_ms = [this]() { return now; };
    screen.preview_done = [this]() { ++preview_done_calls; };
    flow = std::make_unique<ForwardFlow>(presenter, screen);
  }

  void Drain() {
    while (!posted.empty()) {
      auto task = std::move(posted.front());
      posted.pop_front();
      task();
    }
  }

  void Hold(const ThreadMessage& row, const size_t size = 100) {
    rows[row.id] = row;
    auto fields = ChatPayloadCodec::DecodeAttachmentJson(row.payload_json);
    store[fields->content_hash] = ByteVector(size, fields->content_hash[0]);
  }
  void Add(const ThreadMessage& row) { rows[row.id] = row; }

  void PickerSends(const std::vector<std::string>& keys) {
    std::vector<ForwardTarget> chosen;
    for (const ForwardTarget& target : picker_options.list()) {
      if (std::find(keys.begin(), keys.end(), target.Key()) != keys.end()) {
        chosen.push_back(target);
      }
    }
    picker_open = false;
    picker_done(std::move(chosen));
    Drain();
  }

  /** The menu's Select on the first id, a tap on the rest, the bottom button, the picker's Send. */
  void Forward(const std::vector<std::string>& ids, const std::vector<std::string>& keys) {
    flow->BeginSelecting(ids.front());
    for (size_t i = 1; i < ids.size(); ++i) {
      flow->Tap(ids[i]);
    }
    flow->Next();
    ASSERT_TRUE(picker_open);
    PickerSends(keys);
  }

  /** The messaging layer answers the oldest upload: stored in the chat. */
  void AnswerOk() {
    ASSERT_FALSE(uploads.empty());
    Upload upload = std::move(uploads.front());
    uploads.pop_front();
    ThreadMessage stored;
    stored.id = "att-" + std::to_string(++sent_ids);
    stored.thread_id = upload.thread;
    stored.delivery = MessageDelivery::Relayed;
    rows[stored.id] = stored;
    upload.on_done(stored);
    Drain();
    flow->Tick(true);
  }

  struct Timer {
    int64_t at;
    std::function<void()> task;
  };

  ChatPresenter presenter;
  std::unique_ptr<ForwardFlow> flow;
  std::map<std::string, ThreadMessage> rows;
  std::map<std::vector<uint8_t>, ByteVector> store;
  std::vector<ForwardCandidate> candidates;
  std::vector<std::string> log;
  std::deque<Upload> uploads;
  std::deque<std::function<void()>> posted;
  std::vector<Timer> timers;
  std::vector<std::string> pills;
  std::vector<bool> awake;
  std::vector<ForwardProgressView> views;
  bool picker_open = false;
  bool progress_open = false;
  bool attachment_busy = false;
  bool call_active = false;
  int pickers_opened = 0;
  int progress_opened = 0;
  int progress_closed = 0;
  int preview_done_calls = 0;
  int64_t now = 1'000'000;
  size_t sent_ids = 0;
  size_t stats = 0;
  size_t loads = 0;
  PeoplePickerChooseOptions picker_options;
  PeoplePickerChooseDone picker_done;
  std::function<void()> picker_cancel;
};

class ForwardFilesFlowTest : public ::testing::Test, public World {};

// ---- The picker: why Send is off ------------------------------------------------------------------------------------

TEST_F(ForwardFilesFlowTest, OverFiveUploadsOrOverTheSizeLimitDisablesSendAndSaysWhy) {
  for (uint8_t i = 1; i <= 3; ++i) {
    Hold(Picture("p" + std::to_string(i), i, i, 9 * kMb));
  }
  flow->BeginSelecting("p1");
  flow->Tap("p2");
  flow->Tap("p3");
  flow->Next();
  ASSERT_TRUE(picker_open);
  ASSERT_TRUE(static_cast<bool>(picker_options.refusal));
  EXPECT_EQ(picker_options.refusal(0), "") << "no chat chosen yet: nothing is broken";
  EXPECT_EQ(picker_options.refusal(1), Tr("chat.forward.limit_size", {{"n", "27"}})) << "3 x 9 MB is over 25 MB";
  EXPECT_EQ(picker_options.refusal(2), Tr("chat.forward.limit_uploads", {{"n", "6"}})) << "3 pictures x 2 chats is 6";

  // Taking one away leaves two pictures: 18 MB to one chat is fine, 4 uploads to two chats too.
  flow->Leave();
  flow->BeginSelecting("p1");
  flow->Tap("p2");
  flow->Next();
  EXPECT_EQ(picker_options.refusal(1), "");
  EXPECT_EQ(picker_options.refusal(2), Tr("chat.forward.limit_size", {{"n", "36"}}));
}

TEST_F(ForwardFilesFlowTest, ASingleFileOverTheLimitSaysItsOwnLine) {
  Hold(Attachment("big", 1, "film.mov", "video/quicktime", 30 * kMb, 9));
  flow->BeginSingle("big");
  ASSERT_TRUE(picker_open);
  EXPECT_EQ(picker_options.refusal(1), Tr("chat.forward.too_big_single", {{"n", "30"}}));
}

TEST_F(ForwardFilesFlowTest, SendIsRefusedWhileAnotherAttachmentIsBeingSent) {
  Hold(Picture("p1", 1, 1));
  attachment_busy = true;
  flow->BeginSingle("p1");
  ASSERT_TRUE(picker_open);
  EXPECT_EQ(picker_options.refusal(1), Tr("chat.forward.busy")) << "the line above Send, Send greyed by the picker";
  // An answer that slipped through anyway starts nothing: the pill says why and no page opens.
  PickerSends({"t-bob"});
  EXPECT_EQ(pills, (std::vector<std::string>{Tr("chat.forward.busy")}));
  EXPECT_EQ(progress_opened, 0);
  EXPECT_TRUE(uploads.empty());
  EXPECT_TRUE(log.empty());
  EXPECT_FALSE(presenter.Forwarding());
}

TEST_F(ForwardFilesFlowTest, SendIsRefusedDuringACall) {
  // The page would cover the call's controls (the in-progress call is below the full-screen layers): so no forward with
  // an upload begins while a call is on, and the picker says so.
  Hold(Picture("p1", 1, 1));
  call_active = true;
  flow->BeginSingle("p1");
  ASSERT_TRUE(picker_open);
  EXPECT_EQ(picker_options.refusal(1), Tr("chat.forward.call_active"));
  EXPECT_EQ(Tr("chat.forward.call_active").empty(), false);
  PickerSends({"t-bob"});
  EXPECT_EQ(pills, (std::vector<std::string>{Tr("chat.forward.call_active")}));
  EXPECT_EQ(progress_opened, 0);
  EXPECT_TRUE(uploads.empty());
  // Text alone has no page and is allowed during a call.
  Add(Text("t1", "hello", 5));
  pills.clear();
  flow->BeginSingle("t1");
  EXPECT_EQ(picker_options.refusal(1), "");
}

// ---- The page ---------------------------------------------------------------------------------------------------------

TEST_F(ForwardFilesFlowTest, ASelectionWithoutAttachmentsShowsNoProgressPage) {
  Add(Text("t1", "one", 1));
  Add(Text("t2", "two", 2));
  Forward({"t1", "t2"}, {"t-bob"});
  EXPECT_EQ(progress_opened, 0);
  EXPECT_TRUE(awake.empty()) << "the screen is not held for text";
  EXPECT_EQ(log, (std::vector<std::string>{"text:t-bob:one", "text:t-bob:two"}));
  EXPECT_EQ(pills, (std::vector<std::string>{Tr("chat.forward.done")}));
}

TEST_F(ForwardFilesFlowTest, ASinglePictureForwardGoesStraightToThePickerThenTheProgressPage) {
  Hold(Picture("p1", 1, 1));
  flow->BeginSingle("p1");
  EXPECT_TRUE(picker_open) << "no selection bars, the picker at once";
  EXPECT_FALSE(flow->Selecting());
  EXPECT_EQ(progress_opened, 0) << "the page waits for Send";
  PickerSends({"t-bob"});
  EXPECT_EQ(progress_opened, 1);
  EXPECT_TRUE(progress_open);
  EXPECT_EQ(awake, (std::vector<bool>{true}));
  ASSERT_FALSE(views.empty());
  EXPECT_EQ(views.back().title, Tr("chat.forward.progress_title", {{"done", "1"}, {"total", "1"}}));
  EXPECT_EQ(log, (std::vector<std::string>{"att:t-bob:photo-p1.jpg"}));
  AnswerOk();
  EXPECT_FALSE(progress_open);
  EXPECT_EQ(pills, (std::vector<std::string>{Tr("chat.forward.done")}));
}

TEST_F(ForwardFilesFlowTest, TheProgressPageShowsDoneOfTotalAndCancelAndBlocksTheChat) {
  Hold(Picture("p1", 1, 1));
  Hold(Picture("p2", 2, 2));
  Forward({"p1", "p2"}, {"t-bob", "t-cy"});
  ASSERT_TRUE(progress_open);
  EXPECT_EQ(views.back().title, Tr("chat.forward.progress_title", {{"done", "1"}, {"total", "4"}}));
  EXPECT_FALSE(views.back().finishing) << "Cancel is on offer";
  AnswerOk();
  EXPECT_EQ(views.back().title, Tr("chat.forward.progress_title", {{"done", "2"}, {"total", "4"}}));

  // The layer: full screen, no edge-swipe back, and the page is the only thing the user can touch.
  std::vector<PaneSpec> specs;
  ForwardProgressController controller;
  ShellNavigationPorts navigation;
  navigation.push_layer = [&specs](const PaneSpec& spec) {
    specs.push_back(spec);
    return 7;
  };
  controller.BindShellNavigation(navigation);
  controller.Open();
  ASSERT_EQ(specs.size(), 1u);
  EXPECT_EQ(specs[0].key, "forward_progress");
  EXPECT_TRUE(specs[0].full_screen);
  const std::string shell = ReadAll(std::string(PP_SOURCE_DIR) + "/src/gui/shell/ShellHost.cpp");
  EXPECT_EQ(shell.find("forward_progress"), std::string::npos) << "ShellHost lists no swipe-back for the page";
  const std::string rml = ReadAll(std::string(PP_BROWSER_ASSETS_DIR) + "/views/forward_progress.rml");
  EXPECT_NE(rml.find("forward_progress_cancel()"), std::string::npos);
  EXPECT_NE(rml.find("chat.forward.progress_stay"), std::string::npos) << "the line that says leaving stops it";
  EXPECT_EQ(controller.IsOpen(), true);
}

TEST_F(ForwardFilesFlowTest, TheProgressPageShowsActivityForTheCurrentItem) {
  Hold(Picture("p1", 1, 1));
  flow->BeginSingle("p1");
  PickerSends({"t-bob"});
  ASSERT_EQ(uploads.size(), 1u);
  // No report yet: activity and the time the upload has been going.
  now += 7000;
  flow->Tick(true);
  EXPECT_EQ(views.back().percent, -1);
  EXPECT_EQ(views.back().line, Tr("chat.forward.progress_sending", {{"s", "7"}}));
  // The upload reports its size: a percent.
  uploads.front().progress(30, 120);
  flow->Tick(true);
  EXPECT_EQ(views.back().percent, 25);
  EXPECT_EQ(views.back().line, "25%");
  uploads.front().progress(120, 120);
  flow->Tick(true);
  EXPECT_EQ(views.back().percent, 100);

  // The view as a pure function too.
  ForwardProgress p;
  p.done = 1;
  p.total = 7;
  p.uploading = true;
  p.uploading_since_ms = 0;
  EXPECT_EQ(MakeForwardProgressView(p, 3000).title, Tr("chat.forward.progress_title", {{"done", "2"}, {"total", "7"}}));
  EXPECT_EQ(MakeForwardProgressView(p, 3000).line, Tr("chat.forward.progress_sending", {{"s", "3"}}));
  p.uploading = false;
  p.waiting_for_pace = true;
  EXPECT_EQ(MakeForwardProgressView(p, 3000).line, Tr("chat.forward.progress_wait"));
}

TEST_F(ForwardFilesFlowTest, CancelShowsFinishingUntilTheItemInFlightEnds) {
  Hold(Picture("p1", 1, 1));
  Hold(Picture("p2", 2, 2));
  Forward({"p1", "p2"}, {"t-bob"});
  ASSERT_EQ(uploads.size(), 1u);
  flow->OnProgressCancel();
  EXPECT_TRUE(progress_open) << "the page waits for the item that cannot be stopped";
  EXPECT_TRUE(views.back().finishing);
  EXPECT_EQ(views.back().line, Tr("chat.forward.progress_finishing"));
  EXPECT_FALSE(views.back().can_close) << "Close is not offered at once";
  EXPECT_EQ(log.size(), 1u) << "nothing new started";
  EXPECT_TRUE(pills.empty());
  // 15 s with no answer: Close is offered; it leaves without waiting.
  now += kForwardCloseOfferMs + 1;
  flow->Tick(true);
  EXPECT_TRUE(views.back().can_close);
  AnswerOk();
  EXPECT_FALSE(progress_open);
  EXPECT_EQ(pills, (std::vector<std::string>{Tr("chat.forward.partial", {{"done", "1"}, {"total", "2"}})}));
  EXPECT_EQ(log.size(), 1u) << "the second picture never started";
}

TEST_F(ForwardFilesFlowTest, TheProgressPageClosesOnFinishCancelLockAndSignOut) {
  struct Exit {
    const char* name;
    std::function<void(World&)> leave;
  };
  const std::vector<Exit> exits = {
      {"finish", [](World& t) { t.AnswerOk(); }},
      {"cancel",
       [](World& t) {
         t.flow->OnProgressCancel();
         t.AnswerOk();
       }},
      {"lock", [](World& t) { t.flow->Stop(); }},
      {"sign-out", [](World& t) { t.flow->Stop(); }},
      {"layer closed by someone else",
       [](World& t) {
         t.flow->OnProgressLayerClosed();
         t.progress_open = false; // the layer went by itself, the flow does not close it again
         t.AnswerOk();
       }},
      {"phone went to the background", [](World& t) { t.flow->OnAppEnteredBackground(); }},
  };
  for (const Exit& exit : exits) {
    SCOPED_TRACE(exit.name);
    World fresh;
    fresh.Hold(Picture("p1", 1, 1));
    fresh.flow->BeginSingle("p1");
    fresh.PickerSends({"t-bob"});
    ASSERT_TRUE(fresh.progress_open);
    ASSERT_EQ(fresh.awake, (std::vector<bool>{true}));
    exit.leave(fresh);
    EXPECT_FALSE(fresh.progress_open) << "the page is gone";
    EXPECT_EQ(fresh.awake, (std::vector<bool>{true, false})) << "the screen is let go, once";
  }
}

TEST_F(ForwardFilesFlowTest, ThePreviewFilesAreToldToGoWhenThePickerClosesAndOnLockSignOutAndTeardown) {
  // ChatController points preview_done at ForwardPreviewFiles::RemoveAll; lock, sign-out and messaging teardown all end
  // in ForwardFlow::Stop.
  World closed;
  closed.Hold(Picture("p1", 1, 1));
  closed.flow->BeginSingle("p1");
  closed.flow->Leave();
  EXPECT_EQ(closed.preview_done_calls, 1);
  World locked;
  locked.Hold(Picture("p1", 1, 1));
  locked.flow->BeginSingle("p1");
  locked.flow->Stop();
  EXPECT_GE(locked.preview_done_calls, 1);
}

TEST_F(ForwardFilesFlowTest, TheProgressPageKeepsTheScreenAwakeAndReleasesItOnEveryExit) {
  // Every way the page can end, with what each leaves behind: held while it is up, released exactly once after, and a
  // second forward holds it again. (A cancel is answered by the item under way, so its release waits for that.)
  const std::vector<std::pair<const char*, std::function<void(World&)>>> exits = {
      {"completion", [](World& t) { t.AnswerOk(); }},
      {"cancel",
       [](World& t) {
         t.flow->OnProgressCancel();
         t.AnswerOk();
       }},
      {"lock", [](World& t) { t.flow->Stop(); }},
      {"sign-out",
       [](World& t) {
         t.presenter.AbortForward();
         t.flow->Stop();
       }},
      {"layer closed",
       [](World& t) {
         t.flow->OnProgressLayerClosed();
         t.progress_open = false;
       }},
      {"close after a cancel nobody answered",
       [](World& t) {
         t.flow->OnProgressCancel();
         t.now += kForwardCloseOfferMs + 1;
         t.flow->Tick(true);
         t.flow->OnProgressClose();
       }},
  };
  for (const auto& [name, leave] : exits) {
    SCOPED_TRACE(name);
    World fresh;
    fresh.Hold(Picture("p1", 1, 1));
    fresh.flow->BeginSingle("p1");
    fresh.PickerSends({"t-bob"});
    ASSERT_EQ(fresh.awake, (std::vector<bool>{true}));
    leave(fresh);
    EXPECT_EQ(fresh.awake, (std::vector<bool>{true, false}));
    EXPECT_FALSE(fresh.progress_open);
    fresh.flow->Tick(true);
    EXPECT_EQ(fresh.awake.size(), 2u) << "no second release";
  }
}

TEST_F(ForwardFilesFlowTest, BackOrEscOnTheProgressPageCancels) {
  Hold(Picture("p1", 1, 1));
  Hold(Picture("p2", 2, 2));
  Forward({"p1", "p2"}, {"t-bob"});
  ASSERT_TRUE(progress_open);
  EXPECT_TRUE(flow->OnProgressBack()) << "taken: the layer is not closed by the coordinator";
  EXPECT_TRUE(progress_open) << "it waits for the item under way";
  EXPECT_TRUE(views.back().finishing);
  EXPECT_EQ(log.size(), 1u);
  AnswerOk();
  EXPECT_FALSE(progress_open);
  EXPECT_EQ(log.size(), 1u) << "stopped";

  // Through the coordinator's ports, as the back key reaches the controller.
  ForwardProgressController controller;
  int cancels = 0;
  std::function<bool()> step_back;
  std::function<void()> on_cancel;
  ShellNavigationPorts navigation;
  navigation.push_layer = [](const PaneSpec&) { return 3; };
  FlowCoordinatorPorts coordinator;
  coordinator.begin_modal = [&](int, std::function<bool()> back, std::function<void()> cancel) {
    step_back = std::move(back);
    on_cancel = std::move(cancel);
  };
  controller.BindShellNavigation(navigation);
  controller.BindFlowCoordinator(coordinator);
  ForwardProgressController::Handlers handlers;
  handlers.back = [&cancels]() {
    ++cancels;
    return true;
  };
  controller.SetHandlers(handlers);
  controller.Open();
  ASSERT_TRUE(static_cast<bool>(step_back));
  EXPECT_TRUE(step_back()) << "Esc / back is consumed as Cancel";
  EXPECT_EQ(cancels, 1);
}

TEST_F(ForwardFilesFlowTest, ANotificationTapClosingTheLayerStopsWithItsOwnPill) {
  Hold(Picture("p1", 1, 1));
  Hold(Picture("p2", 2, 2));
  Forward({"p1", "p2"}, {"t-bob"});
  flow->OnProgressLayerClosed();
  EXPECT_EQ(progress_closed, 0) << "the layer is gone already; it is not closed a second time";
  AnswerOk();
  EXPECT_EQ(pills, (std::vector<std::string>{Tr("chat.forward.stopped", {{"done", "1"}, {"total", "2"}})}))
      << "worded differently from the user's own Cancel";
}

TEST_F(ForwardFilesFlowTest, ACallStartedWhileThePageIsUpClosesItAndTheRunStops) {
  Hold(Picture("p1", 1, 1));
  Hold(Picture("p2", 2, 2));
  Forward({"p1", "p2"}, {"t-bob"});
  ASSERT_TRUE(progress_open);
  flow->OnCallStarted(); // the call's screen sits under the page: the page goes so it can be seen and ended
  EXPECT_FALSE(progress_open);
  EXPECT_EQ(awake, (std::vector<bool>{true, false}));
  EXPECT_FALSE(presenter.Forwarding());
  EXPECT_EQ(log.size(), 1u) << "nothing new starts";
  AnswerOk(); // the item under way still lands; the notice follows once it did
  flow->Tick(true);
  EXPECT_EQ(pills, (std::vector<std::string>{Tr("chat.forward.partial", {{"done", "1"}, {"total", "2"}})}));
}

TEST_F(ForwardFilesFlowTest, AnIncomingCallIsShownAboveTheProgressPageAndTheRunGoesOn) {
  // The ring's mount sits above the full-screen layers (z 53 against 50), so the ring is on top of the page; a ring is
  // not "a call is on": nothing in the flow reacts to it, and the run goes on.
  const std::string css = ReadAll(std::string(PP_BROWSER_ASSETS_DIR) + "/themes/components.rcss");
  const auto z_of = [&css](const std::string& selector) {
    const size_t at = css.find(selector + " {");
    EXPECT_NE(at, std::string::npos) << selector;
    const size_t z = css.find("z-index:", at);
    return std::stoi(css.substr(z + 8));
  };
  EXPECT_EQ(z_of(".shell-layer-overlay,\n.shell-layer-dialog"), 50);
  EXPECT_GT(z_of(".shell-call-chrome-mount--ring"), z_of(".shell-layer-overlay,\n.shell-layer-dialog"));
  EXPECT_LT(z_of(".shell-call-chrome-mount--bar"), z_of(".shell-layer-overlay,\n.shell-layer-dialog"))
      << "the in-progress call is BELOW the page: that is why a call stops it (above)";

  Hold(Picture("p1", 1, 1));
  Hold(Picture("p2", 2, 2));
  Forward({"p1", "p2"}, {"t-bob"});
  for (int i = 0; i < 5; ++i) {
    now += 1000;
    flow->Tick(true); // the ring is up: ticks go on, the page is not touched
  }
  EXPECT_TRUE(progress_open);
  AnswerOk();
  EXPECT_EQ(log.size(), 2u) << "the run went on with the second picture while the ring was showing";
  AnswerOk();
  EXPECT_EQ(pills, (std::vector<std::string>{Tr("chat.forward.done")}));

  // Only an answered or placed call (in progress) takes the page away; a ring does not, in the controller's wiring.
  const std::string chat = ReadAll(std::string(PP_SOURCE_DIR) + "/src/gui/chat/ChatController.cpp");
  EXPECT_NE(chat.find("forward_flow_->ProgressOpen() && ChromeSnapshot().call_in_progress"), std::string::npos);
  EXPECT_EQ(chat.find("forward_flow_->ProgressOpen() && ChromeSnapshot().call_shown"), std::string::npos);
}

TEST_F(ForwardFilesFlowTest, ASourceChatDeletedElsewhereShowsThePillAtOnceAndTheAppClosesThePage) {
  Hold(Picture("p1", 1, 1));
  Hold(Picture("p2", 2, 2));
  Forward({"p1", "p2"}, {"t-bob"});
  flow->OnThreadGone("src");
  EXPECT_FALSE(progress_open);
  EXPECT_EQ(awake, (std::vector<bool>{true, false}));
  ASSERT_EQ(pills.size(), 1u) << "told at once, not hung on the chat that no longer exists";
}

TEST_F(ForwardFilesFlowTest, ANoticeWaitsForTheItemUnderWayAndSaysOneIsStillSending) {
  Hold(Picture("p1", 1, 1));
  Hold(Picture("p2", 2, 2));
  Forward({"p1", "p2"}, {"t-bob"});
  AnswerOk(); // one went; the second is in flight
  flow->OnAppEnteredBackground();
  flow->Tick(false);
  EXPECT_TRUE(pills.empty()) << "nobody is looking at a chat";
  flow->Tick(true);
  EXPECT_EQ(pills, (std::vector<std::string>{Tr("chat.forward.still_sending")}));
  flow->Tick(true);
  EXPECT_EQ(pills.size(), 1u) << "said once";
  AnswerOk();
  flow->Tick(true);
  ASSERT_EQ(pills.size(), 2u);
  EXPECT_EQ(pills[1], Tr("chat.forward.done")) << "both of its items went";
}

// ---- The screen switch is shared with a call ---------------------------------------------------------------------------

TEST(ScreenAwakeHoldsTest, ReleasingOurScreenHoldDoesNotReleaseAnActiveCallsHold) {
  std::vector<bool> switched;
  ScreenAwakeHolds holds([&switched](const bool on, const char*) { switched.push_back(on); });
  EXPECT_TRUE(holds.Set(ScreenAwakeHolds::Holder::Call, true, "call"));
  EXPECT_FALSE(holds.Set(ScreenAwakeHolds::Holder::Forward, true, "forward"))
      << "already on: the switch is not touched";
  EXPECT_FALSE(holds.Set(ScreenAwakeHolds::Holder::Forward, false, "forward"));
  EXPECT_TRUE(holds.Any()) << "the call still holds";
  EXPECT_EQ(switched, (std::vector<bool>{true})) << "never turned off under the call";
  // And the other way round: the call ends while the page is up.
  EXPECT_FALSE(holds.Set(ScreenAwakeHolds::Holder::Forward, true, "forward"));
  EXPECT_FALSE(holds.Set(ScreenAwakeHolds::Holder::Call, false, "ended"));
  EXPECT_EQ(switched, (std::vector<bool>{true}));
  EXPECT_TRUE(holds.Set(ScreenAwakeHolds::Holder::Forward, false, "forward"));
  EXPECT_EQ(switched, (std::vector<bool>{true, false})) << "off only when nobody holds";
  EXPECT_FALSE(holds.Any());
}

TEST(ScreenAwakeHoldsTest, TheCallControllerGoesThroughTheSharedHolds) {
  const std::string header = ReadAll(std::string(PP_SOURCE_DIR) + "/src/gui/CallController.h");
  EXPECT_NE(header.find("ScreenAwakeHolds::Shared().Set(ScreenAwakeHolds::Holder::Call"), std::string::npos);
  EXPECT_EQ(header.find("ScreenAwake::SetKeepScreenOn(on, screen_awake_reason_)"), std::string::npos)
      << "the call no longer flips the OS switch behind a forward's back";
  const std::string chat = ReadAll(std::string(PP_SOURCE_DIR) + "/src/gui/chat/ChatController.cpp");
  EXPECT_NE(chat.find("ScreenAwakeHolds::Shared().Set(ScreenAwakeHolds::Holder::Forward"), std::string::npos);
}

// ---- The preview ----------------------------------------------------------------------------------------------------

ForwardItemFacts Item(const std::string& id, const ForwardItemKind kind, const uint64_t bytes, const bool here = true,
                      const std::string& filename = "") {
  ForwardItemFacts item;
  item.message_id = id;
  item.kind = kind;
  item.byte_length = bytes;
  item.filename = filename;
  item.content_hash = HashOf(static_cast<uint8_t>(id.back()));
  item.has_local_bytes = here;
  return item;
}

struct PreviewFakes {
  std::map<std::string, std::string> bubble;
  std::map<std::string, std::string> posters;
  bool plaintext_works = true;
  size_t plaintext_calls = 0;
  size_t thumbnail_calls = 0;
  std::vector<size_t> files_put;

  ForwardPreviewPorts Ports() {
    ForwardPreviewPorts ports;
    ports.bubble_picture = [this](const std::string& id) {
      const auto it = bubble.find(id);
      return it == bubble.end() ? std::string() : it->second;
    };
    ports.poster = [this](const ForwardItemFacts& item) {
      const auto it = posters.find(item.message_id);
      return it == posters.end() ? std::string() : it->second;
    };
    ports.plaintext = [this](const std::vector<uint8_t>&) -> Roe<ByteVector> {
      ++plaintext_calls;
      if (!plaintext_works) {
        return Error("not here");
      }
      return ByteVector(10, 1);
    };
    ports.thumbnail = [this](const ByteVector&, int) -> Roe<std::vector<uint8_t>> {
      ++thumbnail_calls;
      return std::vector<uint8_t>{0xFF, 0xD8};
    };
    ports.put_file = [this](const std::vector<uint8_t>&, const size_t index) {
      files_put.push_back(index);
      return "/scratch/" + std::to_string(index) + ".jpg";
    };
    return ports;
  }
};

ForwardCheck CheckOf(std::vector<ForwardItemFacts> items) {
  return CheckForwardLimits(std::move(items), 0, false);
}

const auto kCount = [](const size_t n) { return Tr("chat.forward.preview_count", {{"n", std::to_string(n)}}); };

TEST(ForwardPreviewTilesTest, ThePreviewShowsThePicturesOwnThumbnail) {
  PreviewFakes fakes;
  fakes.bubble["p1"] = "/views/p1.poster.jpg";
  const ForwardPreviewContent content =
      BuildForwardPreview(CheckOf({Item("p1", ForwardItemKind::Picture, 2 * kMb)}), "", kCount, fakes.Ports());
  ASSERT_EQ(content.items.size(), 1u);
  EXPECT_EQ(content.items[0].thumb_src, "/views/p1.poster.jpg") << "the picture the bubble shows";
  EXPECT_EQ(fakes.plaintext_calls, 0u) << "nothing is decrypted when the bubble's picture is there";
  EXPECT_EQ(content.text, "") << "one attachment: the tile says it all";
}

TEST(ForwardPreviewTilesTest, ThePreviewShowsAPosterForAVideo) {
  PreviewFakes fakes;
  fakes.posters["v1"] = "/views/v1.poster.jpg";
  const ForwardPreviewContent content =
      BuildForwardPreview(CheckOf({Item("v1", ForwardItemKind::Video, 5 * kMb)}), "", kCount, fakes.Ports());
  ASSERT_EQ(content.items.size(), 1u);
  EXPECT_EQ(content.items[0].thumb_src, "/views/v1.poster.jpg");
  EXPECT_EQ(content.items[0].icon, "video") << "the icon behind it, shown until the poster is";
  EXPECT_EQ(fakes.plaintext_calls, 0u);
}

TEST(ForwardPreviewTilesTest, ThePreviewShowsAnIconForAFileOrWhenNoThumbnailIsAvailable) {
  PreviewFakes fakes;
  ForwardPreviewContent file = BuildForwardPreview(
      CheckOf({Item("f1", ForwardItemKind::File, 1'200'000, true, "plan.pdf")}), "", kCount, fakes.Ports());
  ASSERT_EQ(file.items.size(), 1u);
  EXPECT_EQ(file.items[0].icon, "file");
  EXPECT_TRUE(file.items[0].thumb_src.empty());
  EXPECT_NE(file.items[0].label.find("plan.pdf"), std::string::npos) << "its name";
  EXPECT_GT(file.items[0].label.size(), std::string("plan.pdf").size()) << "and its size after it";
  EXPECT_EQ(fakes.plaintext_calls, 0u) << "a file is never read for a preview";

  ForwardPreviewContent voice = BuildForwardPreview(
      CheckOf({Item("a1", ForwardItemKind::Voice, 20'000, true, VoiceFileName(7000))}), "", kCount, fakes.Ports());
  EXPECT_EQ(voice.items[0].icon, "mic");
  EXPECT_EQ(voice.items[0].label, Tr("chat.forward.preview_voice", {{"t", "0:07"}}))
      << "the length of the voice message";

  // A picture with no bubble and bytes that cannot be read: the icon, no error.
  fakes.plaintext_works = false;
  const ForwardPreviewContent picture =
      BuildForwardPreview(CheckOf({Item("p9", ForwardItemKind::Picture, kMb)}), "", kCount, fakes.Ports());
  ASSERT_EQ(picture.items.size(), 1u);
  EXPECT_EQ(picture.items[0].icon, "photo");
  EXPECT_TRUE(picture.items[0].thumb_src.empty());
  EXPECT_FALSE(picture.items[0].grey) << "it is here, just not readable now";
}

TEST(ForwardPreviewTilesTest, AnItemWithoutLocalBytesIsGreyAndTheNoteSaysHowManyWillNotGo) {
  PreviewFakes fakes;
  fakes.bubble["p1"] = "/views/p1.jpg";
  const ForwardPreviewContent content = BuildForwardPreview(
      CheckOf({Item("p1", ForwardItemKind::Picture, kMb, false), Item("p2", ForwardItemKind::Picture, kMb, false),
               Item("f3", ForwardItemKind::File, kMb, true, "a.zip")}),
      "", kCount, fakes.Ports());
  ASSERT_EQ(content.items.size(), 3u);
  EXPECT_TRUE(content.items[0].grey);
  EXPECT_TRUE(content.items[1].grey);
  EXPECT_FALSE(content.items[2].grey);
  EXPECT_TRUE(content.items[0].thumb_src.empty()) << "nothing to show of what this device does not have";
  EXPECT_EQ(fakes.plaintext_calls, 0u);
  EXPECT_EQ(content.note, Tr("chat.forward.note_without_bytes", {{"n", "2"}}));
  EXPECT_EQ(content.text, kCount(3)) << "several: \"3 messages\" and the first three tiles";
}

TEST(ForwardPreviewTilesTest, APictureScrolledFarAwayStillGetsAThumbnailFromMemory) {
  PreviewFakes fakes; // no bubble picture: the row is not on the page
  std::vector<ForwardItemFacts> items;
  for (const char* id : {"p1", "p2", "p3", "p4", "p5"}) {
    items.push_back(Item(id, ForwardItemKind::Picture, 2 * kMb));
  }
  const ForwardPreviewContent content = BuildForwardPreview(CheckOf(items), "", kCount, fakes.Ports());
  ASSERT_EQ(content.items.size(), 3u) << "at most three tiles";
  for (size_t i = 0; i < 3; ++i) {
    EXPECT_EQ(content.items[i].thumb_src, "/scratch/" + std::to_string(i) + ".jpg");
  }
  EXPECT_EQ(fakes.plaintext_calls, 3u) << "only the three shown are read, never the other two";
  EXPECT_EQ(fakes.thumbnail_calls, 3u);
  EXPECT_EQ(content.text, kCount(5));
}

TEST(ForwardPreviewTilesTest, APreviewItemOverEightMegabytesShowsAnIcon) {
  PreviewFakes fakes;
  const ForwardPreviewContent content = BuildForwardPreview(
      CheckOf({Item("p1", ForwardItemKind::Picture, 9 * kMb), Item("p2", ForwardItemKind::Picture, 8 * kMb)}), "",
      kCount, fakes.Ports());
  ASSERT_EQ(content.items.size(), 2u);
  EXPECT_TRUE(content.items[0].thumb_src.empty()) << "above 8 MB: the icon";
  EXPECT_EQ(content.items[0].icon, "photo");
  EXPECT_FALSE(content.items[1].thumb_src.empty()) << "exactly 8 MB is still read";
  EXPECT_EQ(fakes.plaintext_calls, 1u) << "the big one is not read at all";
}

TEST(ForwardPreviewTilesTest, TextAndCardsAloneKeepTheWordsAndShowNoTiles) {
  PreviewFakes fakes;
  const ForwardPreviewContent content =
      BuildForwardPreview(CheckOf({Item("t1", ForwardItemKind::Text, 0), Item("c2", ForwardItemKind::Card, 0)}),
                          "2 messages", kCount, fakes.Ports());
  EXPECT_EQ(content.text, "2 messages");
  EXPECT_TRUE(content.items.empty());
  EXPECT_TRUE(content.note.empty());
}

TEST(ForwardPreviewTilesTest, EachRefusalHasItsOwnLine) {
  ForwardCheck check;
  check.uploads = 6;
  check.upload_bytes = 27 * kMb;
  check.largest_bytes = 31 * kMb;
  const struct {
    ForwardRefusal refusal;
    std::string text;
  } cases[] = {
      {ForwardRefusal::None, ""},
      {ForwardRefusal::AttachmentBusy, Tr("chat.forward.busy")},
      {ForwardRefusal::CallActive, Tr("chat.forward.call_active")},
      {ForwardRefusal::TooManyUploads, Tr("chat.forward.limit_uploads", {{"n", "6"}})},
      {ForwardRefusal::TooLarge, Tr("chat.forward.limit_size", {{"n", "27"}})},
      {ForwardRefusal::SingleTooLarge, Tr("chat.forward.too_big_single", {{"n", "31"}})},
  };
  for (const auto& c : cases) {
    check.refusal = c.refusal;
    EXPECT_EQ(ForwardRefusalText(check), c.text);
  }
}

} // namespace
} // namespace pbr
