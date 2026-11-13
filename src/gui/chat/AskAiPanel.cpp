#include "gui/chat/AskAiPanel.h"

#include "foundation/i18n/LocalizationService.h"
#include "foundation/i18n/ScriptLanguageDetector.h"
#include "gui/chat/AgentTurnController.h"
#include "gui/chat/AiImageAttach.h"
#include "gui/chat/ChatAnswer.h"
#include "gui/shell/DataModelHost.h"
#include "lib/ai/StructuredTextParser.h"
#include "lib/ui/ChatFormHelper.h"
#include "lib/ui/ContextMenuHost.h"
#include "lib/ui/DataEvent.h"
#include "common/Logger.h"
#include "common/Utilities.h"

#include <algorithm>
#include <cmath>

#include <ui/data/DataModelHandle.h>
#include <ui/dom/Context.h>
#include <ui/dom/Element.h>
#include <ui/dom/ElementDocument.h>
#include <ui/base/StringUtilities.h>
#include <ui/widgets/ElementFormControlTextArea.h>

namespace pbr {

namespace {

logging::Logger& log() {
  static logging::Logger logger = logging::getLogger("AskAiPanel");
  return logger;
}

constexpr const char* kModel = "ask_ai";
constexpr const char* kScrollId = "ask-ai-scroll";
constexpr const char* kStreamRowId = "ask-ai-stream";
/** New content is scrolled into view over several frames (the rows lay out a frame or two after they change). */
constexpr int kScrollFrames = 8;
constexpr auto kStreamRenderEvery = std::chrono::milliseconds(100);
/** The demo's made-up answer streams at about this many bytes a second (a Chinese character is three). */
constexpr double kFakeBytesPerSecond = 90.0;
/** A made-up failure ends its turn after this long, like a real one that timed out. */
constexpr auto kFakeFailAfter = std::chrono::milliseconds(1500);

std::string AssistantBubble(const std::string& body, const std::string& text_for_lang) {
  return ApplyLangAttribute(R"(<div class="bubble bubble-assistant")", text_for_lang) + R"( selectable="text">)" +
         body + "</div>";
}

std::string UserBubble(const std::string& text) {
  return R"(<div class="bubble bubble-user" selectable="text"><p class="bubble-text">)" +
         StructuredTextParser::EscapeText(text) + "</p></div>";
}

/** Cut at a whole UTF-8 character. */
size_t Utf8Floor(const std::string& text, size_t at) {
  at = std::min(at, text.size());
  while (at > 0 && at < text.size() && (static_cast<unsigned char>(text[at]) & 0xC0) == 0x80) {
    --at;
  }
  return at;
}

AgentEvent Event(const AgentEventType type, const std::string& thread_id) {
  AgentEvent event;
  event.type = type;
  event.thread_id = thread_id;
  return event;
}

} // namespace

AskAiPanel::AskAiPanel(AskAiPanelHost& host, AgentTurnController& agent) : host_(host), agent_(agent) {
  image_ports_.pick = [this](std::function<void(std::string)> picked) { agent_.PickAiImage(std::move(picked)); };
  image_ports_.capture = [this](std::function<void(std::string)> picked) { agent_.CaptureAiPhoto(std::move(picked)); };
  image_ports_.pick_from = [this](const bool files_app, std::function<void(std::string)> picked) {
    agent_.PickAiImageFrom(files_app, std::move(picked));
  };
  image_ports_.prepare = [this](std::string path, std::function<void(Roe<AgentTurnController::PendingAiImage>)> done) {
    agent_.PrepareAiImage(std::move(path), std::move(done));
  };
  image_ports_.send = [this](const std::string& thread_id, const std::string& text,
                             AgentTurnController::PendingAiImage image) {
    return agent_.SendAiImageTo(thread_id, text, std::move(image));
  };
  image_ports_.drop = [this](const AgentTurnController::PendingAiImage& image) { agent_.DropAiImage(image); };
}

bool AskAiPanel::Register(ui::Context* context) {
  context_ = context;
  return DataModelHost::Instance().Register(context, kModel, [this](ui::DataModelConstructor& ctor) {
    if (auto row = ctor.RegisterStruct<AskAiRow>()) {
      row.RegisterMember("id", &AskAiRow::id);
      row.RegisterMember("user", &AskAiRow::user);
      row.RegisterMember("content_rml", &AskAiRow::content_rml);
    }
    ctor.RegisterArray<std::vector<AskAiRow>>();
    ctor.Bind("once", &view_.once);
    ctor.Bind("show_intro", &view_.show_intro);
    ctor.Bind("rows", &view_.rows);
    ctor.Bind("thinking", &view_.thinking);
    ctor.Bind("queued_text", &view_.queued_text);
    ctor.Bind("error", &view_.error);
    ctor.Bind("show_support", &view_.show_support);
    ctor.Bind("draft", &view_.draft);
    ctor.Bind("show_media", &view_.show_media);
    ctor.Bind("has_picture", &view_.has_picture);
    ctor.Bind("picture_preparing", &view_.picture_preparing);
    ctor.Bind("picture_src", &view_.picture_src);
    ctor.BindEventCallback("ask_ai_attach",
                           DataEventAt([this](ui::Event& ev) { ShowAttachMenu(ev.GetCurrentElement()); }));
    ctor.BindEventCallback("ask_ai_emoji", DataEvent([this] {
                             host_.AskAiOpenEmoji([this](const std::string& emoji) { InsertEmoji(emoji); });
                           }));
    ctor.BindEventCallback("ask_ai_remove_picture", DataEvent([this] { RemovePicture(); }));
    ctor.BindEventCallback("ask_ai_send", DataEvent([this] { Send(); }));
    ctor.BindEventCallback("ask_ai_intro_seen", DataEvent([this] { DismissIntro(); }));
    ctor.BindEventCallback("ask_ai_support", DataEvent([this] { AskSupport(); }));
    ctor.BindEventCallback("ask_ai_own_ai_help", DataEvent([this] { AskSupportOwnAi(); }));
    ctor.BindEventCallback("ask_ai_open_in_chat", DataEvent([this] { OpenInChat(); }));
    // The answers' own buttons and links (their markup is the chat's, bound to this model here).
    ctor.BindEventCallback("send_chat_action",
                           DataEvent<std::string, int>(
                               [this](const std::string& entry_id, const int index) { RunAction(entry_id, index); }));
    ctor.BindEventCallback("open_chat_link",
                           DataEvent<std::string, int>(
                               [this](const std::string& entry_id, const int index) { OpenLink(entry_id, index); }));
  });
}

void AskAiPanel::Dirty(const char* field) {
  if (context_) {
    DataModelHost::Instance().Dirty(kModel, field);
  }
}

void AskAiPanel::DirtyAll() {
  if (context_) {
    DataModelHost::Instance().DirtyAll(kModel);
  }
}

bool AskAiPanel::Shows(const std::string& thread_id) const {
  return IsOpen() && !view_.once && !thread_id.empty() && thread_id == shared_thread_;
}

void AskAiPanel::SwitchMode(const bool once) {
  if (view_.once == once) {
    return;
  }
  (view_.once ? once_draft_ : shared_draft_) = view_.draft.c_str();
  view_.once = once;
  view_.draft = (once ? once_draft_ : shared_draft_).c_str();
  view_.show_media = !once;
  SyncPictureView();
}

std::string AskAiPanel::ThreadToSend() {
  if (!fresh_) {
    return host_.AskAiSharedThread(/*create=*/true);
  }
  if (shared_thread_.empty()) {
    shared_thread_ = host_.AskAiNewThread();
  }
  return shared_thread_;
}

bool AskAiPanel::Show(const AskAiEntry entry, const bool once) {
  if (!host_.AskAiCompact()) {
    return false;
  }
  SwitchMode(once);
  if (!once) {
    if (entry == AskAiEntry::Dot) {
      // Opened again while open keeps its new chat; a closed panel starts empty.
      if (!IsOpen()) {
        fresh_ = true;
        shared_thread_.clear();
      }
    } else {
      fresh_ = false;
    }
    if (!fresh_) {
      shared_thread_ = host_.AskAiSharedThread(/*create=*/false);
    }
  }
  view_.show_intro = !host_.AskAiIntroSeen();
  if (!IsOpen()) {
    PaneSpec spec;
    spec.key = "ask_ai";
    // Opens at the height every sheet opens at (Kenneth 2026-11-04).
    spec.page_sheet = true;
    layer_id_ = host_.AskAiPushSheet(spec, [this]() {
      // Closed by the X, the scrim or a swipe: the draft stays, a running answer goes on where it belongs.
      layer_id_ = -1;
    });
    if (layer_id_ < 0) {
      return false;
    }
  }
  Refresh();
  scroll_frames_ = kScrollFrames;
  return true;
}

bool AskAiPanel::Open(const AskAiEntry entry) {
  return Show(entry, /*once=*/false);
}

bool AskAiPanel::OpenAndAsk(const AskAiEntry entry, const std::string& question) {
  if (!Show(entry, /*once=*/false)) {
    return false;
  }
  SendShared(question);
  return true;
}

bool AskAiPanel::OpenOnce(const std::string& question) {
  if (question.empty() || !Show(AskAiEntry::SettingHelp, /*once=*/true)) {
    return false;
  }
  // The same "?" again while its question still shows (asked, answering or answered): nothing is sent twice. One
  // that failed is asked again.
  if (!once_turns_.empty() && once_turns_.front().question == question) {
    const bool failed = once_turns_.back().failed && !agent_.Busy().BusyIn(kAskAiOnceThread);
    if (!failed) {
      return true;
    }
  }
  if (once_pending_ && *once_pending_ == question) {
    return true;
  }
  once_turns_.clear();
  once_links_.clear();
  once_draft_.clear();
  view_.draft = "";
  AskOnce(question, /*fresh=*/true);
  return true;
}

void AskAiPanel::Send() {
  const std::string text = util::Trim(view_.draft.c_str());
  const bool picture = !view_.once && picture_.has_value();
  if ((text.empty() && !picture) || view_.thinking) {
    return;
  }
  if (picture) {
    SendPicture(text);
    return;
  }
  if (view_.once && once_pending_) {
    host_.AskAiToast(Tr("chat.ai_queue.already")); // a question already waits for a free turn: this one stays typed
    return;
  }
  view_.draft = "";
  Dirty("draft");
  if (view_.once) {
    AskOnce(text, /*fresh=*/false);
  } else {
    SendShared(text);
  }
}

// --- the picture (camera / attachment), the chat's own path ----------------------------------------------------

void AskAiPanel::SyncPictureView() {
  const bool shown = !view_.once && (picture_.has_value() || picture_preparing_);
  view_.has_picture = shown;
  view_.picture_preparing = shown && picture_preparing_;
  view_.picture_src = shown && picture_ ? picture_->file_path.c_str() : "";
  Dirty("has_picture");
  Dirty("picture_preparing");
  Dirty("picture_src");
}

void AskAiPanel::DropPicture() {
  ++picture_generation_; // a picture still being prepared is dropped when it arrives
  picture_preparing_ = false;
  if (picture_) {
    if (image_ports_.drop) {
      image_ports_.drop(*picture_);
    }
    picture_.reset();
  }
}

void AskAiPanel::RemovePicture() {
  DropPicture();
  SyncPictureView();
}

void AskAiPanel::PickPicture(const bool camera) {
  if (view_.once) {
    return;
  }
  auto& pick = camera ? image_ports_.capture : image_ports_.pick;
  if (pick) {
    pick([this](std::string path) { PreparePicture(std::move(path)); });
  }
}

void AskAiPanel::ShowAttachMenu(ui::Element* anchor) {
  if (view_.once || !anchor) {
    return;
  }
  const auto pick_from = [this](const bool files_app) {
    return [this, files_app]() {
      if (image_ports_.pick_from) {
        image_ports_.pick_from(files_app, [this](std::string path) { PreparePicture(std::move(path)); });
      }
    };
  };
  ContextMenuHost::Instance().ShowActionsPopoverLeftAligned(
      anchor,
      AgentTurnController::HomeAttachActions([this]() { PickPicture(/*camera=*/true); }, pick_from(false),
                                             pick_from(true)),
      {}, /*above=*/true);
}

void AskAiPanel::InsertEmoji(const std::string& emoji) {
  if (emoji.empty()) {
    return;
  }
  const std::string next = std::string(view_.draft.c_str()) + emoji;
  view_.draft = next.c_str();
  Dirty("draft");
  if (!context_ || context_->GetNumDocuments() == 0) {
    return;
  }
  // The field shows it now (the model copies on the next update); the caret goes to its end.
  if (auto* input =
          ui_dynamic_cast<ui::ElementFormControlTextArea*>(context_->GetDocument(0)->GetElementById("ask-ai-input"))) {
    input->SetValue(next.c_str());
    const ui::String value = input->GetValue();
    const int end = ui::StringUtilities::ConvertByteOffsetToCharacterOffset(value, static_cast<int>(value.size()));
    input->SetSelectionRange(end, end);
  }
}

void AskAiPanel::PreparePicture(std::string path) {
  DropPicture();
  const uint64_t generation = ++picture_generation_;
  picture_preparing_ = true;
  SyncPictureView();
  image_ports_.prepare(std::move(path), [this, generation](Roe<AgentTurnController::PendingAiImage> result) {
    if (generation != picture_generation_) {
      if (result && image_ports_.drop) {
        image_ports_.drop(*result); // removed or replaced while it was being prepared
      }
      return;
    }
    picture_preparing_ = false;
    if (result) {
      picture_ = std::move(*result);
    } else {
      // The cause decides the wording: not an image, too large (the 1.5 MB cap), unreadable.
      host_.AskAiToast(Tr(AiImagePrepErrorKey(AiImagePrepErrorOf(result.error()))));
    }
    SyncPictureView();
  });
}

void AskAiPanel::SendPicture(const std::string& text) {
  if (!picture_) {
    return; // still being prepared
  }
  const std::string question = text.empty() ? Tr("chat.image.default_question") : text;
  const std::string thread_id = ThreadToSend();
  if (thread_id.empty()) {
    FailShared(question);
    Refresh();
    return;
  }
  shared_thread_ = thread_id;
  shared_failed_ = false;
  if (!agent_.UseLlm()) {
    // The demo app: no AI, made-up answers.
    if (!fake_answer_ || host_.AskAiStoreQuestion(thread_id, std::string(kAiImageTurnMarker) + question).empty()) {
      FailShared(question);
    } else {
      StartFake(thread_id, question);
      view_.draft = "";
      Dirty("draft");
      DropPicture();
    }
    Refresh();
    SyncPictureView();
    return;
  }
  // The picture is not queued (it lives in this panel only): with every turn taken it stays, with the text.
  switch (image_ports_.send(thread_id, text, *picture_)) {
    case AgentTurnController::ImageSend::Started:
      awaiting_answer_ = true;
      asked_question_ = question;
      view_.draft = "";
      Dirty("draft");
      picture_preparing_ = false;
      ++picture_generation_;
      picture_.reset(); // the thread's own copy takes over; the controller removes the preview file
      break;
    case AgentTurnController::ImageSend::Full:
      host_.AskAiToast(Tr("chat.ai_queue.image_busy"));
      break;
    case AgentTurnController::ImageSend::BusyHere:
      host_.AskAiToast(Tr("chat.ai_queue.image_busy_here"));
      break;
    case AgentTurnController::ImageSend::Unavailable:
      FailShared(question);
      break;
  }
  Refresh();
  SyncPictureView();
  scroll_frames_ = kScrollFrames;
}

void AskAiPanel::DismissIntro() {
  view_.show_intro = false;
  host_.AskAiMarkIntroSeen();
  Dirty("show_intro");
}

void AskAiPanel::AskSupport() {
  std::string question;
  if (view_.once) {
    question = once_turns_.empty() ? std::string() : once_turns_.front().question;
  } else {
    question = failed_question_;
  }
  if (question.empty()) {
    return;
  }
  host_.AskAiSupport(question, [this] { Close(); });
}

void AskAiPanel::AskSupportOwnAi() {
  host_.AskAiSupport(Tr("ask_ai.intro.support_question"), [this] { Close(); });
}

void AskAiPanel::OpenInChat() {
  const std::string thread_id =
      shared_thread_.empty() && !fresh_ ? host_.AskAiSharedThread(/*create=*/true) : shared_thread_;
  if (thread_id.empty()) {
    return;
  }
  if (IsOpen()) {
    host_.AskAiCloseSheet(layer_id_);
  }
  host_.AskAiOpenInChat(thread_id);
}

void AskAiPanel::Close() {
  if (IsOpen()) {
    host_.AskAiCloseSheet(layer_id_);
  }
}

void AskAiPanel::RunAction(const std::string& entry_id, const int action_index) {
  if (view_.once || shared_thread_.empty()) {
    return; // a one-off answer is stored nowhere: it has no buttons to run
  }
  host_.AskAiRunAction(shared_thread_, entry_id, action_index);
}

void AskAiPanel::OpenLink(const std::string& entry_id, const int link_index) {
  if (const auto it = once_links_.find(entry_id); it != once_links_.end()) {
    if (const std::optional<std::string> url = ResolveChatLink(it->second, link_index)) {
      host_.AskAiOpenUrl(*url);
    }
    return;
  }
  if (!shared_thread_.empty()) {
    host_.AskAiOpenLink(shared_thread_, entry_id, link_index);
  }
}

// --- the shared conversation ----------------------------------------------------------------------------

void AskAiPanel::FailShared(const std::string& question) {
  shared_failed_ = true;
  failed_question_ = question;
  awaiting_answer_ = false;
}

void AskAiPanel::SendShared(const std::string& text) {
  const std::string thread_id = ThreadToSend();
  if (thread_id.empty()) {
    FailShared(text);
    Refresh();
    return;
  }
  shared_thread_ = thread_id;
  shared_failed_ = false;
  // Asked again while the same question waits (a second tap on a chip): it is not queued twice.
  if (agent_.QueuedText(thread_id) == text) {
    Refresh();
    return;
  }
  QueuedAgentSend item;
  item.thread_id = thread_id;
  item.text = text;
  if (!agent_.UseLlm()) {
    if (fake_answer_) {
      if (host_.AskAiStoreQuestion(thread_id, text).empty()) {
        FailShared(text);
      } else {
        StartFake(thread_id, text);
      }
    } else {
      FailShared(text); // no assistant set up: say so, never a silent nothing
    }
    Refresh();
    return;
  }
  if (!agent_.AgentCloudReady()) {
    if (agent_.AgentReady() && !agent_.AgentConfigured() && agent_.AgentHasCredentials()) {
      // Still connecting (just launched or unlocked): it goes out once the assistant is ready.
      if (!agent_.QueueSend(std::move(item))) {
        host_.AskAiToast(Tr("chat.ai_queue.already"));
      }
    } else {
      FailShared(text);
    }
    Refresh();
    return;
  }
  switch (DecideSend(true, agent_.CanStart(thread_id), agent_.Busy().BusyIn(thread_id), agent_.HasQueued(thread_id))) {
    case SendDecision::Send:
      switch (host_.AskAiSendNow(item)) {
        case QueuedSendOutcome::Started:
          awaiting_answer_ = true;
          asked_question_ = text;
          break;
        case QueuedSendOutcome::Busy:
          (void)agent_.QueueSend(std::move(item));
          break;
        case QueuedSendOutcome::NotStarted:
          FailShared(text);
          break;
      }
      break;
    case SendDecision::Queue:
    case SendDecision::BlockedHere:
      // Every turn is taken (a guest has one), or this conversation still answers: it waits, shown as it was asked.
      (void)agent_.QueueSend(std::move(item));
      break;
    case SendDecision::AlreadyQueued:
      // Another question already waits here: this one stays in the input.
      view_.draft = text.c_str();
      Dirty("draft");
      host_.AskAiToast(Tr("chat.ai_queue.already"));
      break;
  }
  Refresh();
  scroll_frames_ = kScrollFrames;
}

// --- the one-off question (settings "?") ----------------------------------------------------------------

void AskAiPanel::AskOnce(const std::string& text, const bool fresh) {
  once_pending_ = text;
  once_pending_fresh_ = fresh;
  if (!TryStartOnce()) {
    Refresh();
  }
}

bool AskAiPanel::TryStartOnce() {
  if (!once_pending_) {
    return false;
  }
  const std::string text = *once_pending_;
  if (!agent_.UseLlm()) {
    once_pending_.reset();
    once_turns_.push_back({.question = text});
    if (fake_answer_) {
      StartFake(kAskAiOnceThread, text);
    } else {
      once_turns_.back().failed = true;
    }
    Refresh();
    return true;
  }
  if (!agent_.AgentCloudReady() && !(agent_.AgentReady() && agent_.AgentHasCredentials())) {
    once_pending_.reset();
    once_waits_for_turn_ = false;
    once_turns_.push_back({.question = text, .failed = true});
    Refresh();
    return true;
  }
  // The sessions' cap counts it like a chat's question (a guest has one turn): with every turn taken it waits, shown
  // as waiting ("AI is busy, sent later"), and goes out when one is free (Tick).
  const bool full = agent_.CanStart(kAskAiOnceThread) == TurnStart::Full;
  if (full != once_waits_for_turn_) {
    once_waits_for_turn_ = full;
    Refresh();
  }
  if (full) {
    return false;
  }
  AgentUiPorts& ports = agent_.Ports();
  if (once_pending_fresh_) {
    // A new "?": nothing said before goes along. An earlier one-off still answering is stopped first; the
    // question waits here (Tick) until the Home session is free.
    if (!ports.start_new_home_conversation || !ports.start_new_home_conversation()) {
      if (ports.cancel && agent_.Busy().BusyIn(kAskAiOnceThread) && !once_cancel_sent_) {
        ports.cancel({});
        once_cancel_sent_ = true;
      }
      return false;
    }
  }
  const SubmitOutcome outcome = agent_.SubmitAskAiOnce(text);
  if (outcome == SubmitOutcome::BusyHere) {
    return false; // its previous turn is still leaving: Tick tries again
  }
  once_pending_.reset();
  once_cancel_sent_ = false;
  once_waits_for_turn_ = false;
  once_turns_.push_back({.question = text, .failed = outcome != SubmitOutcome::Started});
  if (outcome != SubmitOutcome::Started) {
    log().warning << "One-off question not started (outcome " << static_cast<int>(outcome) << ")";
  }
  Refresh();
  scroll_frames_ = kScrollFrames;
  return true;
}

void AskAiPanel::OnOnceReply(const AgentEvent& event) {
  if (once_turns_.empty() || once_turns_.back().done) {
    return; // a one-off replaced by a newer "?" (its turn was stopped): dropped
  }
  OnceTurn& turn = once_turns_.back();
  const std::string entry_id = "ask-ai-once-" + std::to_string(once_turns_.size());
  std::string body;
  if (event.render_mode == RenderMode::Markdown) {
    ChatAnswerRml answer = BuildMarkdownAnswer(event.text);
    body = InjectEntryPlaceholders(answer.rml, entry_id);
    once_links_[entry_id] = std::move(answer.links);
  } else {
    const ParseResult parsed =
        event.from_model ? StructuredTextParser::ParseFromLlmOutput(event.text, event.response_goal, event.render_mode)
                         : StructuredTextParser::ParseBlocksJson(event.text, event.response_goal, event.render_mode);
    body = parsed.ok ? InjectEntryPlaceholders(parsed.rml, entry_id)
                     : "<p>" + StructuredTextParser::EscapeText(StructuredTextParser::PlainText(event.text)) + "</p>";
  }
  turn.answer_rml = AssistantBubble(body, event.text);
  turn.done = true;
  Refresh();
  scroll_frames_ = kScrollFrames;
}

void AskAiPanel::OnTurnEnded(const std::string& thread_id) {
  if (thread_id == kAskAiOnceThread) {
    if (!once_turns_.empty() && !once_turns_.back().done) {
      once_turns_.back().failed = true; // ended with no answer: an error, said in plain words
      once_turns_.back().done = true;
    }
    Refresh();
    return;
  }
  if (thread_id.empty() || thread_id != shared_thread_) {
    return;
  }
  if (awaiting_answer_ && !agent_.Busy().BusyIn(thread_id) && host_.AskAiLastIsQuestion(thread_id)) {
    FailShared(asked_question_); // the turn ended with no answer: plain words, and the question to a person
  }
  awaiting_answer_ = false;
  Refresh();
  scroll_frames_ = kScrollFrames;
}

// --- the demo's made-up answers ----------------------------------------------------------------------------

void AskAiPanel::StartFake(const std::string& thread_id, const std::string& question) {
  fake_ = FakeStream{.thread_id = thread_id,
                     .answer = fake_answer_ ? fake_answer_(question) : std::string(),
                     .started = std::chrono::steady_clock::now()};
  if (thread_id != kAskAiOnceThread) {
    awaiting_answer_ = true;
    asked_question_ = question;
  }
  AgentEvent loading = Event(AgentEventType::LoadingChanged, thread_id);
  loading.loading = true;
  agent_.HandleAgentEvent(loading);
}

void AskAiPanel::TickFake() {
  if (!fake_) {
    return;
  }
  const auto elapsed = std::chrono::steady_clock::now() - fake_->started;
  const std::string thread_id = fake_->thread_id;
  if (fake_->answer.empty()) {
    if (elapsed >= kFakeFailAfter) {
      fake_.reset();
      AgentEvent end = Event(AgentEventType::LoadingChanged, thread_id);
      agent_.HandleAgentEvent(end);
    }
    return;
  }
  const double seconds = std::chrono::duration<double>(elapsed).count();
  const size_t upto = Utf8Floor(fake_->answer, static_cast<size_t>(seconds * kFakeBytesPerSecond));
  if (upto < fake_->answer.size()) {
    if (upto > fake_->shown) {
      fake_->shown = upto;
      AgentEvent delta = Event(AgentEventType::AssistantDelta, thread_id);
      delta.entry_id = "ask-ai-fake";
      delta.text = fake_->answer.substr(0, upto);
      agent_.HandleAgentEvent(delta);
    }
    return;
  }
  const std::string answer = fake_->answer;
  fake_.reset();
  AgentEvent ready = Event(AgentEventType::AssistantReady, thread_id);
  ready.text = answer;
  ready.render_mode = RenderMode::Markdown;
  ready.from_model = false;
  agent_.HandleAgentEvent(ready);
  agent_.HandleAgentEvent(Event(AgentEventType::LoadingChanged, thread_id));
}

// --- what shows -------------------------------------------------------------------------------------

void AskAiPanel::Refresh() {
  base_rows_.clear();
  if (view_.once) {
    int n = 0;
    for (const OnceTurn& turn : once_turns_) {
      ++n;
      base_rows_.push_back({("ask-ai-once-q-" + std::to_string(n)).c_str(), true, UserBubble(turn.question).c_str()});
      if (!turn.answer_rml.empty()) {
        base_rows_.push_back({("ask-ai-once-" + std::to_string(n)).c_str(), false, turn.answer_rml.c_str()});
      }
    }
    // A question waiting for a free turn shows as queued; one waiting for the earlier one-off to stop, as asked.
    const bool queued = once_pending_ && once_waits_for_turn_;
    if (once_pending_ && !queued) {
      base_rows_.push_back({"ask-ai-once-pending", true, UserBubble(*once_pending_).c_str()});
    }
    const bool failed = !once_turns_.empty() && once_turns_.back().failed;
    const bool running = once_pending_.has_value() || agent_.Busy().BusyIn(kAskAiOnceThread);
    view_.error = failed && !running;
    view_.queued_text = queued ? once_pending_->c_str() : "";
    // Under the answer (or the error): the same question to a person.
    view_.show_support = !once_turns_.empty() && (once_turns_.front().done || failed) && !running;
  } else {
    if (!shared_thread_.empty()) {
      base_rows_ = host_.AskAiRecentRows(shared_thread_, kRowLimit);
    }
    const bool busy = !shared_thread_.empty() && agent_.Busy().BusyIn(shared_thread_);
    view_.queued_text = shared_thread_.empty() ? "" : agent_.QueuedText(shared_thread_).c_str();
    view_.error = shared_failed_ && !busy;
    view_.show_support = view_.error && !failed_question_.empty();
  }
  streamed_text_.clear();
  RefreshStreaming(/*force=*/true);
  DirtyAll();
}

void AskAiPanel::RefreshStreaming(const bool force) {
  const std::string thread_id = view_.once ? std::string(kAskAiOnceThread) : shared_thread_;
  const std::string text = thread_id.empty() ? std::string() : agent_.StreamingText(thread_id);
  const auto now = std::chrono::steady_clock::now();
  if (!force) {
    if (text == streamed_text_) {
      return;
    }
    if (text.empty()) {
      Refresh(); // the stream ended: the stored answer (or the error) takes its place
      return;
    }
    if (last_stream_render_ && now - *last_stream_render_ < kStreamRenderEvery) {
      return;
    }
  }
  streamed_text_ = text;
  last_stream_render_ = now;
  view_.rows = base_rows_;
  if (!text.empty()) {
    ChatAnswerRml answer = BuildMarkdownAnswer(text);
    view_.rows.push_back(
        {kStreamRowId, false, AssistantBubble(InjectEntryPlaceholders(answer.rml, kStreamRowId), text).c_str()});
    view_.thinking = false; // the text itself shows the AI is at work
  } else if (view_.once) {
    view_.thinking = (once_pending_ && !once_waits_for_turn_) || agent_.Busy().BusyIn(kAskAiOnceThread);
  } else {
    view_.thinking = !shared_thread_.empty() && agent_.Busy().BusyIn(shared_thread_);
  }
  if (!force) {
    Dirty("rows");
    Dirty("thinking");
    scroll_frames_ = kScrollFrames;
  }
}

void AskAiPanel::Tick() {
  TickFake();
  if (once_pending_) {
    (void)TryStartOnce(); // a new "?" waiting for the Home session's previous turn to leave
  }
  if (!IsOpen()) {
    return;
  }
  // "Thinking", the queue and the streamed text follow the conversation shown.
  if (!view_.once) {
    const ui::String queued = shared_thread_.empty() ? "" : agent_.QueuedText(shared_thread_).c_str();
    if (queued != view_.queued_text) {
      Refresh(); // it went out (or waits now): its bubble moves from the queue into the rows
    }
  }
  RefreshStreaming(/*force=*/false);
  if (!context_ || context_->GetNumDocuments() == 0) {
    return;
  }
  ui::ElementDocument* doc = context_->GetDocument(0);
  SyncComposerClear();
  if (scroll_frames_ > 0) {
    --scroll_frames_;
    if (ui::Element* scroll = doc->GetElementById(kScrollId)) {
      scroll->SetScrollTop(scroll->GetScrollHeight());
    }
  }
}

void AskAiPanel::SyncComposerClear() {
  ui::ElementDocument* doc = context_->GetDocument(0);
  ui::Element* bar = doc->GetElementById("ask-ai-composer");
  ui::Element* clear = doc->GetElementById("ask-ai-scroll-end");
  ui::Element* scroll = doc->GetElementById(kScrollId);
  if (!bar || !clear || !scroll) {
    return;
  }
  const float bar_px = bar->GetOffsetHeight();
  if (bar_px <= 0.f || std::abs(clear->GetOffsetHeight() - bar_px) < 0.5f) {
    return;
  }
  const float ratio =
      context_->GetDensityIndependentPixelRatio() > 0.f ? context_->GetDensityIndependentPixelRatio() : 1.f;
  // At the newest bubble (or not yet laid out) it stays there as the bar grows; read further up, it stays put.
  const float from_end = scroll->GetScrollHeight() - scroll->GetClientHeight() - scroll->GetScrollTop();
  clear->SetProperty("height", std::to_string(bar_px / ratio) + "dp");
  if (from_end <= 64.f * ratio) {
    scroll_frames_ = std::max(scroll_frames_, 2);
  }
}

} // namespace pbr
