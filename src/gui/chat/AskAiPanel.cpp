#include "gui/chat/AskAiPanel.h"

#include "foundation/i18n/LocalizationService.h"
#include "foundation/i18n/ScriptLanguageDetector.h"
#include "gui/chat/AgentTurnController.h"
#include "gui/chat/AiImageAttach.h"
#include "gui/chat/ChatAnswer.h"
#include "gui/shell/DataModelHost.h"
#include "lib/ui/ChatFormHelper.h"
#include "lib/ui/ContextMenuHost.h"
#include "lib/ui/DataEvent.h"
#include "common/Utilities.h"

#include <algorithm>
#include <cmath>

#include <ui/data/DataModelHandle.h>
#include <ui/dom/Context.h>
#include <ui/base/SystemInterface.h>
#include <ui/dom/Element.h>
#include <ui/dom/ElementText.h>
#include <ui/dom/SelectionController.h>
#include <ui/dom/ElementDocument.h>
#include <ui/base/StringUtilities.h>
#include <ui/widgets/ElementFormControlTextArea.h>

namespace pbr {

namespace {

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
/** Just unlocked: the PP 客服 question waits this long for messaging, then the sheet says to try again later. */
constexpr int64_t kSupportReadyWaitMs = 5'000;
/** While a PP 客服 reply is awaited (up to kSupportSlowAfterMs), the inbox is fetched this often. */
constexpr int64_t kSupportFetchEveryMs = 15'000;

/** The user's words of `error`, or the "PP 客服 cannot be reached" sentence when it has none. */
std::string SupportWords(const Error& error) {
  return error.user.empty() ? Tr("settings.support.unavailable") : error.user;
}

std::string AssistantBubble(const std::string& body, const std::string& text_for_lang) {
  return ApplyLangAttribute(R"(<div class="bubble bubble-assistant")", text_for_lang) + R"( selectable="text">)" +
         body + "</div>";
}

/** Cut at a whole UTF-8 character. */
size_t Utf8Floor(const std::string& text, size_t at) {
  at = std::min(at, text.size());
  while (at > 0 && at < text.size() && (static_cast<unsigned char>(text[at]) & 0xC0) == 0x80) {
    --at;
  }
  return at;
}

bool IsBlockTag(const std::string& tag) {
  static const char* const kBlocks[] = {"p",  "div", "li",  "ul", "ol", "h1",         "h2",   "h3",
                                        "h4", "h5",  "pre", "tr", "br", "blockquote", "table"};
  return std::any_of(std::begin(kBlocks), std::end(kBlocks), [&tag](const char* block) { return tag == block; });
}

void AppendText(std::string& out, const std::string& text) {
  out += text;
}

void EndLine(std::string& out) {
  if (!out.empty() && out.back() != '\n') {
    out += '\n';
  }
}

void CollectText(ui::Element* element, std::string& out, const bool keep_space) {
  if (!element) {
    return;
  }
  if (auto* text = ui_dynamic_cast<ui::ElementText*>(element)) {
    const std::string raw = ui::StringUtilities::DecodeRml(text->GetText()).c_str();
    if (keep_space) {
      AppendText(out, raw);
      return;
    }
    // Source whitespace collapses to one space, as the engine draws it.
    for (const char c : raw) {
      if (c == ' ' || c == '\n' || c == '\t' || c == '\r') {
        if (!out.empty() && out.back() != ' ' && out.back() != '\n') {
          out += ' ';
        }
      } else {
        out += c;
      }
    }
    return;
  }
  const std::string tag = element->GetTagName().c_str();
  const bool block = IsBlockTag(tag);
  if (block) {
    EndLine(out);
  }
  const bool keep = keep_space || tag == "pre";
  for (int i = 0; i < element->GetNumChildren(); ++i) {
    CollectText(element->GetChild(i), out, keep);
  }
  if (block) {
    EndLine(out);
  }
}

AgentEvent Event(const AgentEventType type, const std::string& thread_id) {
  AgentEvent event;
  event.type = type;
  event.thread_id = thread_id;
  return event;
}

} // namespace

AskAiPanel::AskAiPanel(AskAiPanelHost& host, AgentTurnController& agent)
    : host_(host), agent_(agent), now_ms_([] { return util::NowUnixMs(); }) {
  ContextMenuHost::Instance().SetTextMenuOverride(
      [this](const ContextMenuRequest& request) { return TextMenuFor(request); });
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

AskAiPanel::~AskAiPanel() {
  ContextMenuHost::Instance().SetTextMenuOverride(nullptr);
}

std::string AskAiPanel::PlainTextOf(ui::Element* row) {
  std::string out;
  CollectText(row, out, /*keep_space=*/false);
  while (!out.empty() && (out.back() == '\n' || out.back() == ' ')) {
    out.pop_back();
  }
  size_t start = 0;
  while (start < out.size() && (out[start] == '\n' || out[start] == ' ')) {
    ++start;
  }
  return out.substr(start);
}

std::optional<std::vector<ContextMenuAction>> AskAiPanel::TextMenuFor(const ContextMenuRequest& request) {
  if (!IsOpen() || !request.target) {
    return std::nullopt;
  }
  ui::Element* row = nullptr;
  ui::Element* scroll = nullptr;
  for (ui::Element* element = request.target; element; element = element->GetParentNode()) {
    if (!row && element->IsClassSet("ask-ai-row")) {
      row = element;
    }
    if (element->GetId() == kScrollId) {
      scroll = element;
      break;
    }
  }
  if (!row || !scroll) {
    return std::nullopt;
  }
  std::string text;
  if (request.context) {
    if (ui::SelectionController* selection = request.context->GetSelectionController()) {
      text = selection->GetSelectedText().c_str();
    }
  }
  if (text.empty()) {
    text = PlainTextOf(row);
  }
  std::vector<ContextMenuAction> actions;
  actions.push_back({"copy", Tr("common.copy"), [text]() { return !text.empty(); },
                     [text]() {
                       if (ui::SystemInterface* system = ui::GetSystemInterface()) {
                         system->SetClipboardText(text);
                       }
                     }});
  return actions;
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
    ctor.Bind("support", &view_.support);
    ctor.Bind("support_waiting", &view_.support_waiting);
    ctor.Bind("support_slow", &view_.support_slow);
    ctor.Bind("support_note", &view_.support_note);
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
  if (!IsOpen() || thread_id.empty()) {
    return false;
  }
  if (view_.support) {
    return thread_id == support_.thread;
  }
  return thread_id == shared_thread_;
}

std::string& AskAiPanel::ModeDraft() {
  return view_.support ? support_.draft : shared_draft_;
}

void AskAiPanel::SwitchMode(const bool support) {
  if (view_.support == support) {
    return;
  }
  ModeDraft() = view_.draft.c_str();
  view_.support = support;
  view_.draft = ModeDraft().c_str();
  view_.show_media = !support;
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

bool AskAiPanel::Show(const AskAiEntry entry, const bool support) {
  if (!host_.AskAiCompact()) {
    return false;
  }
  if (IsOpen() && view_.support != support) {
    Close(); // AI and PP 客服 never switch in place: the open sheet goes and the other one opens anew
  }
  SwitchMode(support);
  if (!support) {
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
  view_.show_intro = !support && !host_.AskAiIntroSeen();
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
  return Show(entry, /*support=*/false);
}

bool AskAiPanel::OpenAndAsk(const AskAiEntry entry, const std::string& question) {
  if (!Show(entry, /*support=*/false)) {
    return false;
  }
  SendShared(question);
  return true;
}

bool AskAiPanel::OpenSupport(const std::string& question) {
  if (!host_.AskAiCompact()) {
    return false;
  }
  if (!Show(AskAiEntry::SettingHelp, /*support=*/true)) {
    return false;
  }
  StartSupport(question);
  return true;
}

void AskAiPanel::Send() {
  const std::string text = util::Trim(view_.draft.c_str());
  const bool picture = !view_.support && picture_.has_value();
  if ((text.empty() && !picture) || view_.thinking) {
    return;
  }
  if (view_.support) {
    if (support_.thread.empty()) {
      return; // PP 客服 could not be reached (the sheet says why): the text stays typed
    }
    view_.draft = "";
    Dirty("draft");
    AskSupportQuestion(text, /*typed=*/true);
    return;
  }
  if (picture) {
    SendPicture(text);
    return;
  }
  view_.draft = "";
  Dirty("draft");
  SendShared(text);
}

// --- the picture (camera / attachment), the chat's own path ----------------------------------------------------

void AskAiPanel::SyncPictureView() {
  const bool shown = !view_.support && (picture_.has_value() || picture_preparing_);
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
  if (view_.support) {
    return;
  }
  auto& pick = camera ? image_ports_.capture : image_ports_.pick;
  if (pick) {
    pick([this](std::string path) { PreparePicture(std::move(path)); });
  }
}

void AskAiPanel::ShowAttachMenu(ui::Element* anchor) {
  if (view_.support || !anchor) {
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
  const std::string question = failed_question_;
  if (question.empty()) {
    return;
  }
  // After this tap: the AI sheet closes and the PP 客服 one opens anew (OpenSupport), no AI row stays.
  host_.AskAiLater([this, question] { (void)OpenSupport(question); });
}

void AskAiPanel::AskSupportOwnAi() {
  host_.AskAiLater([this] { (void)OpenSupport(Tr("ask_ai.intro.support_question")); });
}

void AskAiPanel::OpenInChat() {
  const std::string thread_id = view_.support                       ? support_.thread
                                : shared_thread_.empty() && !fresh_ ? host_.AskAiSharedThread(/*create=*/true)
                                                                    : shared_thread_;
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
  if (view_.support || shared_thread_.empty()) {
    return; // the AI's answers' buttons only
  }
  host_.AskAiRunAction(shared_thread_, entry_id, action_index);
}

void AskAiPanel::OpenLink(const std::string& entry_id, const int link_index) {
  if (view_.support || shared_thread_.empty()) {
    return; // the AI's answers' links only (opened without asking); a person's links are opened from the chat page
  }
  host_.AskAiOpenLink(shared_thread_, entry_id, link_index);
}

// --- PP 客服 (a settings "?") -------------------------------------------------------------------------------

void AskAiPanel::StartSupport(const std::string& question) {
  support_.pending.reset();
  view_.support_note = "";
  if (!host_.AskAiMessagingReady()) {
    // Just unlocked: nothing is found, sent or queued now; asked once messaging is up (TickSupport), seconds at most.
    support_.pending = question;
    support_.ready_by_ms = now_ms_() + kSupportReadyWaitMs;
    view_.support_note = Tr("ask_ai.support.preparing").c_str();
    Refresh();
    return;
  }
  Roe<std::string> thread = host_.AskAiEnsureSupport();
  if (!thread) {
    support_.thread.clear();
    view_.support_note = SupportWords(thread.error()).c_str();
    Refresh();
    return;
  }
  support_.thread = *thread;
  ReadSupportIfLooking(); // what came before shows here now
  Refresh();
  host_.AskAiSyncInbox();
  if (!question.empty()) {
    AskSupportQuestion(question, /*typed=*/false);
  }
  scroll_frames_ = kScrollFrames;
}

void AskAiPanel::AskSupportQuestion(const std::string& text, const bool typed) {
  const int64_t now = now_ms_();
  if (!typed && !ShouldAutoSend(support_.messages, text, now)) {
    support_.awaiting = true; // asked moments ago: the line follows that question's reply
    SyncSupportWait();
    return;
  }
  if (Roe<void> sent = host_.AskAiSendToSupport(support_.thread, text); !sent) {
    if (typed) {
      view_.draft = text.c_str();
      Dirty("draft");
    }
    view_.support_note = SupportWords(sent.error()).c_str();
    Dirty("support_note");
    return;
  }
  support_.awaiting = true;
  support_.fetch_until_ms = now + kSupportSlowAfterMs;
  support_.next_fetch_ms = now + kSupportFetchEveryMs;
  view_.support_note = "";
  Refresh();
  host_.AskAiSyncInbox();
  scroll_frames_ = kScrollFrames;
}

void AskAiPanel::SyncSupportWait() {
  const SupportWait wait = support_.awaiting ? ReplyState(support_.messages, now_ms_()) : SupportWait::Answered;
  if (wait == SupportWait::Answered) {
    support_.awaiting = false;
  }
  const bool waiting = wait == SupportWait::Waiting;
  const bool slow = wait == SupportWait::Slow;
  if (waiting == view_.support_waiting && slow == view_.support_slow) {
    return;
  }
  view_.support_waiting = waiting;
  view_.support_slow = slow;
  Dirty("support_waiting");
  Dirty("support_slow");
  scroll_frames_ = kScrollFrames;
}

void AskAiPanel::TickSupport() {
  if (!IsOpen() || !view_.support) {
    return;
  }
  const int64_t now = now_ms_();
  if (support_.pending) {
    if (host_.AskAiMessagingReady()) {
      const std::string question = *support_.pending;
      StartSupport(question); // the one retry
    } else if (now >= support_.ready_by_ms) {
      support_.pending.reset();
      view_.support_note = Tr("ask_ai.support.try_later").c_str();
      Dirty("support_note");
    }
    return;
  }
  if (!support_.awaiting) {
    return;
  }
  SyncSupportWait();
  if (support_.awaiting && now < support_.fetch_until_ms && now >= support_.next_fetch_ms) {
    support_.next_fetch_ms = now + kSupportFetchEveryMs;
    host_.AskAiSyncInbox();
  }
}

void AskAiPanel::ReadSupportIfLooking() {
  if (ShouldReadSupportThread(host_.AskAiAttentive(), Shows(support_.thread))) {
    host_.AskAiMarkRead(support_.thread);
  }
}

void AskAiPanel::OnMessagesChanged() {
  if (!IsOpen() || !view_.support || support_.thread.empty()) {
    return;
  }
  ReadSupportIfLooking(); // a reply that shows here leaves no unread in the chats list
  Refresh();
  scroll_frames_ = kScrollFrames;
}

bool AskAiPanel::SuppressesNoticeFor(const std::string& thread_id) const {
  return view_.support && ShouldSuppressNotice(host_.AskAiAttentive(), Shows(thread_id));
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

void AskAiPanel::OnTurnEnded(const std::string& thread_id) {
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
  awaiting_answer_ = true;
  asked_question_ = question;
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
  if (view_.support) {
    base_rows_.clear();
    support_.messages.clear();
    if (!support_.thread.empty()) {
      base_rows_ = host_.AskAiRecentRows(support_.thread, kRowLimit);
      support_.messages = host_.AskAiSupportMessages(support_.thread, kRowLimit);
    }
    view_.queued_text = "";
    view_.error = false;
    view_.show_support = false;
    SyncSupportWait();
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
  const std::string thread_id = view_.support ? std::string() : shared_thread_;
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
  } else if (view_.support) {
    view_.thinking = false; // PP 客服 is a person's chat: the waiting line says it is replying
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
  TickSupport();
  if (!IsOpen()) {
    return;
  }
  // "Thinking", the queue and the streamed text follow the conversation shown.
  if (!view_.support) {
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
