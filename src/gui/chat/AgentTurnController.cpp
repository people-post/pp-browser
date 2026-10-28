#include "gui/chat/AgentTurnController.h"
#include "lib/ai/WorkingSetPolicy.h"
#include "lib/ai/LocalizedLabels.h"
#include "common/chat/PeopleDiscoveryBlocks.h"
#include "common/ValueJson.h"

#include <chrono>
#include <thread>
#include <stdexcept>
#include <filesystem>
#include <fstream>
#include <utility>
#include "gui/chat/DeliveryNoticeText.h"
#include "gui/chat/TranscriptRml.h"
#include "gui/chat/AttachmentBackfillPolicy.h"
#include "common/Metrics.h"
#include "feature/conversations/ConversationsFacade.h"
#include "gui/shell/ShellSetupPorts.h"
#include "gui/chat/ChatDataModel.h"
#include "gui/chat/AiImageAttach.h"
#include "gui/chat/ApprovalCard.h"
#include "gui/chat/ChatAnswer.h"
#include "feature/conversations/SessionListText.h"
#include "feature/conversations/SessionMenu.h"
#include "gui/chat/BriefFeedIntent.h"
#include "gui/chat/SuggestionPayload.h"
#include "gui/chat/ChatWidgetHost.h"
#include "gui/BadgeAggregator.h"
#include "gui/BadgeNotifyPorts.h"
#include "foundation/i18n/LocalizationService.h"
#include "foundation/i18n/ScriptLanguageDetector.h"
#include "foundation/runtime/AppLifecycle.h"
#include "foundation/runtime/AppRuntime.h"
#include "foundation/platform/ui/DesktopWindowChrome.h"
#include "foundation/platform/ILocalNotifier.h"
#include "foundation/platform/Platform.h"
#include "foundation/platform/IPushDeviceRegistrar.h"
#include "foundation/platform/NativeFileDialog.h"
#include "lib/messaging/DeleteForEveryone.h"
#include "lib/messaging/AttachmentCache.h"
#include "lib/messaging/PlainTextLinks.h"
#include "lib/messaging/QuoteReply.h"
#include "foundation/platform/PlatformOpenFile.h"
#include "foundation/platform/PlatformOpenUrl.h"
#include "lib/ai/MarkdownToRml.h"
#include "lib/ai/StructuredTextParser.h"
#include "lib/ui/ChatFormHelper.h"
#include "common/ui/WorkingSetCodec.h"
#include "foundation/platform/ui/RmlUi_Backend.h"
#include "common/chat/MessagingLimits.h"
#include "gui/shell/DataModelHost.h"
#include "gui/UserFeedback.h"
#include "foundation/data/LlmPreset.h"
#include "foundation/error/AppError.h"
#include "lib/net/BriefGuestLlmClient.h"
#include "foundation/platform/DeploymentProfile.h"

namespace pbr {

namespace {

std::string ToolActivityLabel(const std::string& tool_name, const std::string& status) {
  // brief_AI status events: tool = backend tool name, status = phase.
  if (tool_name == "search_web") {
    return Tr("chat.status.searching_web");
  }
  if (tool_name == "read_url") {
    return Tr("chat.status.reading_page");
  }
  if (tool_name == "search_opensearch") {
    return Tr("chat.status.searching_nfsc");
  }
  if (tool_name == "get_quote") {
    return Tr("chat.status.getting_quote");
  }
  if (tool_name == "blog_articles") {
    // The briefs feed: never show the raw tool name to the user.
    if (status == "running") {
      return Tr("chat.tool.fetching_briefs");
    }
    if (status == "done") {
      return Tr("chat.tool.briefs_fetched");
    }
  }
  if (tool_name == "web_search") {
    if (status == "running") {
      return Tr("chat.tool.searching_web");
    }
    if (status == "done") {
      return Tr("chat.tool.search_complete");
    }
    if (status == "error") {
      return Tr("chat.tool.search_failed");
    }
  }
  if (status == "running") {
    return Tr("chat.tool.running", {{"tool", tool_name}});
  }
  if (status == "done") {
    return Tr("chat.tool.finished", {{"tool", tool_name}});
  }
  return tool_name;
}

/** Result of preparing an image on a worker: the bytes for the request plus a thumbnail file for the UI. */
struct PreparedDraft {
  BriefAiImage image;
  std::string file_path;
  int width = 0;
  int height = 0;
};

Roe<PreparedDraft> PrepareAiImageDraft(const std::string& path, const std::string& profile_dir) {
  auto prepared = PrepareAiImageFromFile(path);
  if (!prepared) {
    return prepared.error();
  }
  PreparedDraft draft;
  draft.image.mime = prepared->mime;
  draft.image.data = prepared->bytes;
  draft.width = prepared->width;
  draft.height = prepared->height;
  if (!profile_dir.empty()) {
    const std::filesystem::path dir = AttachmentViewRoot(profile_dir, kAiImageViewThread);
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const std::filesystem::path file = dir / (util::GenerateUuid() + (prepared->mime == "image/png" ? ".png" : ".jpg"));
    if (!ec) {
      std::ofstream out(file, std::ios::binary | std::ios::trunc);
      out.write(reinterpret_cast<const char*>(prepared->bytes.data()), static_cast<std::streamsize>(prepared->bytes.size()));
      out.close();
      if (out) {
        draft.file_path = file.string();
      } else {
        std::filesystem::remove(file, ec);
      }
    }
  }
  return draft;
}

void RemoveSessionFile(const std::string& path) {
  if (!path.empty()) {
    std::error_code ec;
    std::filesystem::remove(path, ec);
  }
}

} // namespace

AgentTurnController::AgentTurnController(AgentTurnHost& host, ChatScreenState& chat,
                                         std::map<std::string, std::vector<std::string>>& chat_links,
                                         ChatWidgetHost& widgets, WorkingSetController& working_set,
                                         ShellFeedbackPorts& shell_feedback)
    : host_(host), chat_(chat), chat_links_(chat_links), widgets_(widgets), working_set_(working_set),
      shell_feedback_(shell_feedback) {
  redirectLogger("ChatController");
}

void AgentTurnController::Tick() {
  FlushStreamingRow();
  FlushHeldAgentSend();
  if (AgentReady() && agent_ports_.poll_events) {
    std::vector<AgentEvent> events;
    agent_ports_.poll_events(events);
    for (const AgentEvent& event : events) {
      HandleAgentEvent(event);
    }
  }
  RecoverMissedTurnEnd();
}

void AgentTurnController::RecoverMissedTurnEnd() {
  // Every session path that ends a turn should say so (LoadingChanged false); if one ever does not, the open thread
  // would show "thinking" forever and every other AI send would queue for nothing. A safety net, logged.
  constexpr auto kBusyRecoveryDelay = std::chrono::seconds(2);
  if (!busy_.busy || !use_llm_ || !agent_ports_.is_busy || agent_ports_.is_busy()) {
    busy_without_session_since_.reset();
    return;
  }
  const auto now = std::chrono::steady_clock::now();
  if (!busy_without_session_since_) {
    busy_without_session_since_ = now;
    return;
  }
  if (now - *busy_without_session_since_ < kBusyRecoveryDelay) {
    return;
  }
  log().warning << "AI busy without a running turn for 2 s: the turn's end was not heard; recovering";
  busy_without_session_since_.reset();
  MarkAgentIdle();
  DrainQueue();
}

void AgentTurnController::Cancel() {
  if (AgentReady() && agent_ports_.cancel) {
    agent_ports_.cancel();
  }
}

void AgentTurnController::Reset() {
  held_agent_send_.Clear();
  streaming_.reset();
  use_llm_ = false;
}

void AgentTurnController::BindPorts(AgentUiPorts ports) {
  agent_ports_ = std::move(ports);
}

bool AgentTurnController::AgentReady() const {
  return agent_ports_.has_session && agent_ports_.has_session();
}

bool AgentTurnController::AgentConfigured() const {
  if (agent_ports_.snapshot) {
    return agent_ports_.snapshot().configured;
  }
  return false;
}

bool AgentTurnController::AgentCloudReady() const {
  if (!AgentReady() || !host_.Store().IsInitialized()) {
    return false;
  }
  const AppConfig& config = host_.Store().Snapshot().config;
  // Mock replies (no base_url) are always usable once the agent session exists.
  if (config.llm.base_url.empty()) {
    return true;
  }
  if (agent_ports_.snapshot) {
    if (!agent_ports_.snapshot().cloud_ready) {
      return false;
    }
  } else if (!AgentConfigured()) {
    return false;
  }
  return AgentHasCredentials();
}

bool AgentTurnController::AgentHasCredentials() const {
  const AppConfig& config = host_.Store().Snapshot().config;
  if (ResolvePreset(config) == "brief") {
    std::string registered;
    std::string guest;
    if (host_.MessagingReady() && host_.Facade()) {
      if (auto identity = host_.Facade()->GetIdentity()) {
        registered = identity->brief_llm_api_key;
        guest = identity->brief_llm_guest_api_key;
      }
    }
    std::string brief_key = ResolveBriefLlmApiKey(registered, guest);
    if (brief_key.empty()) {
      AppConfig last_probe;
      last_probe.llm = last_agent_runtime_.llm;
      if (ResolvePreset(last_probe) == "brief") {
        brief_key = last_agent_runtime_.llm.api_key;
      }
    }
    return !brief_key.empty();
  }
  if (config.llm.require_api_key) {
    return !config.llm.api_key.empty() || !last_agent_runtime_.llm.api_key.empty();
  }
  return true;
}

void AgentTurnController::SendSharedAssistantRelay(const std::string& thread_id, const AtAiMode mode,
                                              const std::string& plain_text) {
  if (!host_.MessagingIsReady() || plain_text.empty()) {
    return;
  }
  auto thread = host_.Facade()->GetThread(thread_id);
  if (!thread || !*thread) {
    return;
  }

  SendRelayOptions opts;
  opts.sender_contact_id = kAiAssistantContactId;
  opts.generation = "ai_on_behalf";
  opts.ai_invoke_mode = mode == AtAiMode::SharedFull ? "shared_full" : "shared_reply";
  opts.update_preview = true;
  if ((*thread)->kind == ThreadKind::Group) {
    (void)host_.Facade()->SendGroupMessage(thread_id, plain_text, opts);
    return;
  }
  if (!(*thread)->participant_contact_ids.empty()) {
    opts.seq_owner_contact_id = (*thread)->participant_contact_ids.front();
  }
  (void)host_.Facade()->SendUserMessage(thread_id, plain_text, opts);
}

void AgentTurnController::OnAttachAiImage() {
  NativeFileDialogCallback on_picked = [this](std::vector<std::string> paths) {
    AppRuntime::PostUI([this, paths = std::move(paths)]() mutable {
      if (paths.empty()) {
        return;
      }
      StartAiImagePrepare(std::move(paths.front()));
    });
  };
  if (!Platform::IsMobile()) {
    ShowOpenImageFileDialog(Backend::GetWindow(), std::move(on_picked), /*include_heic=*/true);
    return;
  }
  // Same two ways as a chat; the assistant reads images only, so "File" offers image files.
  host_.ShowAttachChoice([on_picked]() { ShowOpenImageFileDialog(Backend::GetWindow(), on_picked, /*include_heic=*/true); },
                   [on_picked]() { ShowOpenImageDocumentDialog(Backend::GetWindow(), on_picked, /*include_heic=*/true); });
}

void AgentTurnController::StartAiImagePrepare(std::string path) {
  DiscardPendingAiImage();
  const uint64_t generation = ++image_prepare_generation_;
  std::string name = std::filesystem::path(path).filename().string();
  chat_.image_chip = true;
  chat_.image_preparing = true;
  chat_.image_thumb_ready = false;
  chat_.image_thumb_src = "";
  chat_.image_draft_name = name.c_str();
  host_.SyncComposerInputState();
  DirtyChatChrome();
  DirtyChatHeader();

  const std::string profile_dir = host_.Facade() ? host_.Facade()->ProfileDataDir() : std::string();
  // Decoding and re-encoding a photo is blocking work: off the UI thread, reply on it.
  AppRuntime::PostWorkerNormal([this, path = std::move(path), profile_dir, generation, name = std::move(name)]() mutable {
    Roe<PreparedDraft> result = PrepareAiImageDraft(path, profile_dir);
    ReleasePickedFile(path); // the draft keeps its own copy
    AppRuntime::PostUI([this, generation, name = std::move(name), result = std::move(result)]() mutable {
      if (generation != image_prepare_generation_) {
        if (result) {
          RemoveSessionFile(result->file_path); // removed or replaced while it was being prepared
        }
        return;
      }
      chat_.image_preparing = false;
      if (!result) {
        // Sizes and kinds only in the log: no file name, no path.
        log().warning << "AI image not prepared: kind=" << result.error().code;
        chat_.image_chip = false;
        chat_.image_draft_name = "";
        host_.SyncComposerInputState();
        DirtyChatChrome();
        DirtyChatHeader();
        UserFeedback::Fail(Tr(AiImagePrepErrorKey(AiImagePrepErrorOf(result.error()))));
        return;
      }
      log().info << "AI image ready: " << result->image.data.size() << " bytes";
      pending_image_ = PendingAiImage{
          .image = std::move(result->image), .name = std::move(name),
          .file_path = std::move(result->file_path),
          .width = result->width,
          .height = result->height};
      chat_.image_thumb_ready = !pending_image_->file_path.empty();
      chat_.image_thumb_src = pending_image_->file_path.c_str();
      host_.SyncComposerInputState();
      DirtyChatChrome();
      DirtyChatHeader();
    });
  });
}

void AgentTurnController::DiscardPendingAiImage() {
  ++image_prepare_generation_;
  if (pending_image_) {
    RemoveSessionFile(pending_image_->file_path);
    pending_image_.reset();
  }
  if (!chat_.image_chip && !chat_.image_preparing) {
    return;
  }
  chat_.image_chip = false;
  chat_.image_preparing = false;
  chat_.image_thumb_ready = false;
  chat_.image_thumb_src = "";
  chat_.image_draft_name = "";
  host_.SyncComposerInputState();
  DirtyChatChrome();
  DirtyChatHeader();
}

void AgentTurnController::StashPendingImage(const std::string& key) {
  if (chat_.image_preparing) {
    DiscardPendingAiImage(); // still being prepared: nothing to keep yet
    return;
  }
  if (!pending_image_) {
    return;
  }
  ForgetPendingImage(key); // at most one kept per conversation
  stashed_images_.emplace(key, std::move(*pending_image_));
  pending_image_.reset();
  chat_.image_chip = false;
  chat_.image_thumb_ready = false;
  chat_.image_thumb_src = "";
  chat_.image_draft_name = "";
  host_.SyncComposerInputState();
  DirtyChatChrome();
  DirtyChatHeader();
}

void AgentTurnController::RestorePendingImage(const std::string& key) {
  auto it = stashed_images_.find(key);
  if (it == stashed_images_.end()) {
    return;
  }
  DiscardPendingAiImage();
  pending_image_ = std::move(it->second);
  stashed_images_.erase(it);
  chat_.image_chip = true;
  chat_.image_preparing = false;
  chat_.image_thumb_ready = !pending_image_->file_path.empty();
  chat_.image_thumb_src = pending_image_->file_path.c_str();
  chat_.image_draft_name = pending_image_->name.c_str();
  host_.SyncComposerInputState();
  DirtyChatChrome();
  DirtyChatHeader();
}

void AgentTurnController::ForgetPendingImage(const std::string& key) {
  auto it = stashed_images_.find(key);
  if (it == stashed_images_.end()) {
    return;
  }
  RemoveSessionFile(it->second.file_path);
  stashed_images_.erase(it);
}

void AgentTurnController::ForgetAllPendingImages() {
  for (auto& [key, image] : stashed_images_) {
    RemoveSessionFile(image.file_path);
  }
  stashed_images_.clear();
  DiscardPendingAiImage();
}

bool AgentTurnController::SendImageQuestion(const std::string& text) {
  if (!host_.MessagingIsReady() || !host_.Facade() || !pending_image_ || !agent_ports_.submit_image_to_thread) {
    return false;
  }
  if (!AgentCloudReady()) {
    RefreshLlmSetupBanner();
    return false;
  }
  if (busy_.busy) {
    // A picture is not queued (it lives in this composer only): the chip and the question stay for later.
    UserFeedback::Fail(Tr("chat.ai_queue.image_busy"));
    return false;
  }
  // Taken out first: opening the AI thread from Home switches threads, which discards a pending image.
  PendingAiImage image = std::move(*pending_image_);
  pending_image_.reset();
  if (host_.ChromeSnapshot().nav_tab == NavTab::Home && !host_.EnsureHomeOutboundSession()) {
    pending_image_ = std::move(image);
    return false;
  }
  chat_.image_chip = false;
  chat_.image_thumb_ready = false;
  chat_.image_thumb_src = "";
  chat_.image_draft_name = "";

  const std::string thread_id = host_.ActiveThreadId();
  const std::string question = text.empty() ? Tr("chat.image.default_question") : text;
  const std::string message_id = util::GenerateUuid();
  widgets_.ExpireOpenForms();
  DirtyChatTurns();
  chat_.status = "";
  MarkAgentBusy(thread_id);
  (void)host_.Facade()->NameAiThreadFromFirstMessage(thread_id, AiThreadTitleFromMessage(question));
  host_.UpdateSidebarPreview(question);
  DirtyChatChrome();
  DirtyChatHeader();
  // The image is kept like a received attachment (private CAS + this thread's view file) and the message
  // stores a reference to it, so the bubble can show it after a restart. Hashing, encrypting and writing
  // ~1.5 MB is blocking work: off the UI thread, submit from the reply. If the store fails the question
  // still goes out and the bubble shows only the marker.
  ConversationsFacade* facade = host_.Facade();
  AppRuntime::PostWorkerNormal([this, facade, thread_id, question, message_id, image = std::move(image)]() mutable {
    auto stored = facade->StoreAiImage(thread_id, image.image.data, image.image.mime);
    AppRuntime::PostUI([this, thread_id, question, message_id, image = std::move(image),
                        stored = std::move(stored)]() mutable {
      std::string ai_image_json;
      if (stored) {
        ai_image_json = AiImageRefToJson(AiImageRef{.content_hash = std::move(stored->content_hash),
                                                    .mime = image.image.mime,
                                                    .width = image.width,
                                                    .height = image.height});
      } else {
        log().warning << "AI image not stored: " << AppError::Log(stored.error());
      }
      RemoveSessionFile(image.file_path); // the composer's preview; the thread's view file takes over
      if (!agent_ports_.submit_image_to_thread) {
        return;
      }
      log().info << "Submitting an image question to the agent session";
      agent_ports_.submit_image_to_thread(thread_id, question,
                                          AgentImageTurn{.image = std::move(image.image),
                                                         .message_id = message_id,
                                                         .ai_image_json = std::move(ai_image_json)});
    });
  });
  return true;
}

void AgentTurnController::DecorateAiImageRows(std::vector<MessageDisplayRow>& rows) const {
  // Only the user's own questions in an AI thread carry the marker; a peer's or the AI's text that
  // happens to start with it is left alone.
  if (!host_.Facade()) {
    return;
  }
  auto thread = host_.Facade()->GetActiveThread();
  if (!thread || thread->kind != ThreadKind::Ai) {
    return;
  }
  for (MessageDisplayRow& row : rows) {
    if (row.row_class != "message-row-user") {
      continue;
    }
    std::string rml = row.content_rml.c_str();
    if (rml.find(kAiImageTurnMarker) == std::string::npos) {
      continue;
    }
    // The message references the stored image (private CAS); the view file is materialized on demand.
    // Without a reference (older questions, a failed store) or while locked only the marker remains.
    std::string src;
    int width = 0;
    int height = 0;
    if (const auto ref = AiImageRefFromJson(row.ai_image_json)) {
      if (const auto view = host_.Facade()->AiImageViewPath(thread->id, ref->content_hash, ref->mime)) {
        src = StructuredTextParser::EscapeText(*view);
        width = ref->width;
        height = ref->height;
      }
    }
    row.content_rml = DecorateAiImageBubble(std::move(rml), src, width, height, Tr("chat.image.marker")).c_str();
  }
}

void AgentTurnController::OnAssistantDelta(const AgentEvent& event) {
  if (!host_.MessagingIsReady()) {
    return;
  }
  if (!streaming_ || streaming_->row_id != "streaming-" + event.entry_id) {
    ClearStreamingRow();
    streaming_ = StreamingRow{};
    streaming_->row_id = "streaming-" + event.entry_id;
    streaming_->thread_id = event.thread_id;
  }
  streaming_->text = event.text;
  streaming_->dirty = true;
  FlushStreamingRow();
}

void AgentTurnController::FlushStreamingRow() {
  if (!streaming_) {
    return;
  }
  bool changed = false;
  const auto now = std::chrono::steady_clock::now();
  if (streaming_->dirty && ShouldRenderStreamDelta(streaming_->last_render, now)) {
    ChatAnswerRml answer = BuildMarkdownAnswer(streaming_->text);
    streaming_->rml = ApplyLangAttribute(R"(<div class="bubble bubble-assistant")", streaming_->text) +
                      R"( selectable="text">)" + InjectEntryPlaceholders(answer.rml, streaming_->row_id) + "</div>";
    chat_links_[streaming_->row_id] = std::move(answer.links);
    streaming_->last_render = now;
    streaming_->dirty = false;
    changed = true;
  }
  if (streaming_->rml.empty() || streaming_->thread_id != host_.ActiveThreadId()) {
    return;
  }
  // Row rebuilds (scroller paging, thread sync) drop the synthetic row; put it back.
  if (chat_.messages.empty() || std::string(chat_.messages.back().message_id.c_str()) != streaming_->row_id) {
    MessageDisplayRow row;
    row.message_id = streaming_->row_id.c_str();
    row.row_class = "message-row-ai";
    chat_.messages.push_back(std::move(row));
    changed = true;
  }
  if (changed) {
    chat_.messages.back().content_rml = streaming_->rml.c_str();
    DirtyChatTurns();
  }
}

void AgentTurnController::ClearStreamingRow() {
  if (!streaming_) {
    return;
  }
  chat_links_.erase(streaming_->row_id);
  streaming_.reset();
}

void AgentTurnController::OnStopTurn() {
  // The stop key only shows in the thread whose turn runs; a stale tap from another thread stops nothing.
  if (!busy_.BusyIn(host_.ActiveThreadId())) {
    log().debug << "Stop ignored: no turn runs in the open thread";
    return;
  }
  if (AgentReady() && agent_ports_.cancel) {
    agent_ports_.cancel();
  }
}

void AgentTurnController::MarkAgentBusy(const std::string& thread_id) {
  busy_.busy = true;
  if (!thread_id.empty()) {
    busy_.thread_id = thread_id;
  } else if (busy_.thread_id.empty()) {
    log().warning << "AI busy without a thread: every thread's AI sends queue until it ends";
  }
  RefreshBusyForActiveThread();
}

void AgentTurnController::MarkAgentIdle() {
  busy_ = {};
  RefreshBusyForActiveThread();
}

void AgentTurnController::RefreshBusyForActiveThread() {
  const std::string& thread_id = host_.ActiveThreadId();
  // "Thinking" only in the conversation whose turn runs; if that is unknown, nowhere (AI sends still queue).
  const bool here = busy_.busy && !busy_.thread_id.empty() && busy_.thread_id == thread_id;
  chat_.loading = here;
  if (!here) {
    chat_.status = "";
  }
  const QueuedAgentSend* queued = queue_.Find(thread_id);
  chat_.has_queued = queued != nullptr;
  chat_.busy_elsewhere = busy_.BusyElsewhere(thread_id);
  chat_.queued_text = queued ? queued->text.c_str() : "";
  host_.ShellSetActivity(here, here ? chat_.status : ui::String{});
  DirtyChatChrome();
}

bool AgentTurnController::QueueSend(QueuedAgentSend item) {
  if (!queue_.Enqueue(std::move(item))) {
    return false;
  }
  log().info << "AI send queued: another thread's turn runs (" << queue_.Size() << " waiting)";
  RefreshBusyForActiveThread();
  if (!busy_.busy) {
    DrainQueue(); // the turn ended while a dialog was open (e.g. the "@ai+" confirm): nothing else would start it
  }
  return true;
}

std::optional<QueuedAgentSend> AgentTurnController::TakeQueued(const std::string& thread_id) {
  auto item = queue_.Take(thread_id);
  if (item) {
    RefreshBusyForActiveThread();
  }
  return item;
}

void AgentTurnController::DrainQueue() {
  if (draining_ || busy_.busy) {
    return;
  }
  draining_ = true;
  while (!busy_.busy) {
    std::optional<QueuedAgentSend> item = queue_.PopFront();
    if (!item) {
      break;
    }
    const QueuedSendOutcome outcome = host_.RunQueuedAgentSend(*item);
    if (outcome == QueuedSendOutcome::Started) {
      break; // the next one waits for this turn to end
    }
    if (outcome == QueuedSendOutcome::Busy) {
      log().info << "Queued AI send put back: the assistant is still busy";
      queue_.PushFront(std::move(*item));
      break;
    }
    log().info << "Queued AI send did not start a turn; going on with the next one";
  }
  draining_ = false;
  RefreshBusyForActiveThread();
}

void AgentTurnController::SubmitForm(const std::string& entry_id, const std::string& form_id) {
  if (chat_.loading) {
    return;
  }
  const auto submission = widgets_.TrySubmit(entry_id, form_id);
  if (!submission) {
    log().warning << "Ignoring submit for inactive or expired form: " << entry_id << "/" << form_id;
    return;
  }
  working_set_.Clear();
  host_.SyncDisplayFromThread();
  host_.SendUserText(submission->display_text, submission->payload);
}

void AgentTurnController::CalendarPrev(const std::string& entry_id) {
  if (!widgets_.ShiftCalendar(entry_id, -1)) {
    return;
  }
  if (working_set_.ActiveEntryId() == entry_id) {
    working_set_.SyncWidgetBindings(entry_id);
  }
  host_.SyncDisplayFromThread();
}

void AgentTurnController::CalendarNext(const std::string& entry_id) {
  if (!widgets_.ShiftCalendar(entry_id, 1)) {
    return;
  }
  if (working_set_.ActiveEntryId() == entry_id) {
    working_set_.SyncWidgetBindings(entry_id);
  }
  host_.SyncDisplayFromThread();
}

void AgentTurnController::SelectCalendarDay(const std::string& entry_id, const std::string& iso_date) {
  if (chat_.loading) {
    return;
  }
  if (!widgets_.IsCalendarDayAvailable(entry_id, iso_date)) {
    return;
  }
  host_.SendUserText("Selected " + iso_date);
}

std::string AgentTurnController::HydrateAssistantRml(const TranscriptEntry& entry) const {
  return widgets_.HydrateAssistantRml(entry);
}

bool AgentTurnController::IsFormEditable(const std::string& entry_id, const std::string& form_id) const {
  return widgets_.IsFormEditable(entry_id, form_id);
}

void AgentTurnController::InitializeWidgetState(const std::string& entry_id, const std::vector<WidgetInit>& inits) {
  widgets_.Initialize(entry_id, inits);
}

void AgentTurnController::MergeWidgetStateIntoRow(const std::string& entry_id, TranscriptDisplayRow& row) const {
  widgets_.MergeIntoRow(entry_id, row);
}

TurnWidgetState* AgentTurnController::FindWidgetState(const std::string& entry_id) {
  return widgets_.Find(entry_id);
}

const TurnWidgetState* AgentTurnController::FindWidgetState(const std::string& entry_id) const {
  return widgets_.Find(entry_id);
}

void AgentTurnController::ClearFormState() {
  widgets_.ClearForms();
}

void AgentTurnController::SendChatAction(const std::string& entry_id, int action_index) {
  if (chat_.loading || action_index < 0 || !host_.MessagingIsReady()) {
    return;
  }

  std::optional<TranscriptChatAction> resolved = working_set_.LookupChatAction(entry_id, action_index);
  if (!resolved) {
    const std::string thread_id = host_.ActiveThreadId();
    auto messages = host_.Facade()->GetMessagesPage(thread_id, std::nullopt, 10000);
    if (messages) {
      for (const ThreadMessage& message : *messages) {
        if (message.id != entry_id) {
          continue;
        }
        if (action_index >= static_cast<int>(message.chat_actions.size())) {
          log().warning << "Chat action index out of range: entry=" << entry_id << " index=" << action_index
                        << " size=" << message.chat_actions.size();
          return;
        }
        resolved = message.chat_actions[static_cast<size_t>(action_index)];
        break;
      }
    }
  }

  if (!resolved) {
    log().warning << "Chat action entry not found: " << entry_id << " thread=" << host_.ActiveThreadId()
                  << " working_set_entry=" << working_set_.ActiveEntryId();
    return;
  }

  // "Show more" of a people list reveals more rows of the same list in place: no new turn, no network.
  // Deferred like the actions below: the re-render replaces the button that was clicked.
  if (const auto payload = resolved->payload ? TryParseObject(*resolved->payload) : std::nullopt;
      payload && payload->getString("type").value_or("") == kShowMoreRowsActionType) {
    const size_t block_index = payload->getNonNegInt("block").value_or(0);
    const size_t step = payload->getNonNegInt("step").value_or(0);
    AppRuntime::PostUI([this, entry_id, block_index, step]() { ShowMoreRows(entry_id, block_index, step); });
    return;
  }

  // Copy by value: layout-mutating actions close the working set / remount panes. Doing that
  // synchronously destroys the click target and leaves RmlUi data views with dangling aliases
  // ("Variable address not found" → segfault). Defer like SettingsController section open.
  const std::string action_message = resolved->message;
  const std::optional<std::string> action_payload = resolved->payload;
  const bool close_working_set = working_set_.ShouldCloseForAction(action_payload);
  AppRuntime::PostUI([this, action_message, action_payload, close_working_set, entry_id]() {
    if (close_working_set) {
      working_set_.Clear();
    }
    host_.HandleLocalAction(action_message, action_payload, entry_id);
  });
}

void AgentTurnController::ShowMoreRows(const std::string& entry_id, const size_t block_index, const size_t step) {
  if (chat_.loading || !host_.MessagingIsReady()) {
    return;
  }
  const std::string thread_id = host_.ActiveThreadId();
  auto found = host_.Facade()->GetMessage(thread_id, entry_id);
  if (!found || !found->has_value()) {
    return;
  }
  ThreadMessage message = **found;
  auto expanded = RevealMoreRowsInBlocksJson(message.text, block_index, step, LocalizedPeopleDiscoveryLabels());
  if (!expanded) {
    log().warning << "Show more: no hidden rows in entry=" << entry_id << " block=" << block_index;
    return;
  }
  if (expanded->size() > kMaxComposeTextBytes) {
    log().warning << "Show more: expanded text too large for the store, entry=" << entry_id;
    return;
  }
  const ParseResult parsed =
      StructuredTextParser::ParseBlocksJson(*expanded, InferResponseGoalFromBlocksJson(*expanded));
  if (!parsed.ok) {
    log().warning << "Show more: re-render failed for entry=" << entry_id << ": " << parsed.error;
    return;
  }

  // Re-render this message only, as FinishAssistantReply stored it (bubble, actions, panel snapshot); the
  // stored document carries the new `visible`, so the expansion survives a restart. No turn, no preview.
  std::vector<TranscriptChatAction> chat_actions;
  chat_actions.reserve(parsed.chat_actions.size());
  for (const ParsedChatAction& action : parsed.chat_actions) {
    chat_actions.push_back({action.label, action.message, action.payload});
  }
  const std::vector<WorkingSetCandidate> candidates =
      working_set_.HydrateCandidates(parsed.working_set_candidates, entry_id);
  std::string hydrated = InjectEntryPlaceholders(parsed.rml, entry_id);
  if (candidates.empty()) {
    hydrated = HydrateChatActionButtons(hydrated, chat_actions);
  }
  message.text = std::move(*expanded);
  message.content_rml = ApplyLangAttribute(R"(<div class="bubble bubble-assistant")", message.text) +
                        R"( selectable="text">)" + hydrated + "</div>";
  message.chat_actions = chat_actions;
  message.working_set_json =
      candidates.empty() ? std::nullopt : std::optional<std::string>(WorkingSetCandidatesToJson(candidates));
  if (auto updated = host_.Facade()->UpdateMessage(message); !updated) {
    log().warning << "Show more: could not store entry=" << entry_id << ": " << updated.error().message;
    return;
  }

  working_set_.ApplyFromParse(entry_id, candidates, std::move(chat_actions));
  host_.SyncDisplayFromThread();
  DirtyChat();
}

void AgentTurnController::CloseApprovalCard(const std::string& thread_id, const std::string& given_entry_id,
                                            const ApprovalOutcome outcome, const std::string& approval_id) {
  if (!host_.MessagingIsReady()) {
    return;
  }
  const std::string target_thread = thread_id.empty() ? host_.ActiveThreadId() : thread_id;
  // A shared "@ai+" card was stored by the UI, so the session does not know its id: find it by its request.
  const std::string entry_id =
      !given_entry_id.empty() ? given_entry_id : FindApprovalCardEntry(*host_.Facade(), target_thread, approval_id);
  if (entry_id.empty()) {
    log().info << "Permission card not found to rewrite (thread has no card for that request)";
    return;
  }
  if (!CloseApprovalCardInStore(*host_.Facade(), target_thread, entry_id, outcome)) {
    log().warning << "Permission card not rewritten: entry=" << entry_id;
    return;
  }
  working_set_.ApplyFromParse(entry_id, {}, {}); // the old buttons must not resolve any more
  if (target_thread == host_.ActiveThreadId()) {
    host_.SyncDisplayFromThread();
  }
  DirtyChat();
}

void AgentTurnController::FinishAssistantReply(const std::string& entry_id, const std::string& raw_output, const bool from_llm,
                                    const std::string& finish_reason, const std::string& thread_id,
                                    ResponseGoal response_goal, RenderMode render_mode, const AtAiMode shared_ai_mode,
                                    const std::vector<BriefAiSource>& sources) {
  const bool markdown = render_mode == RenderMode::Markdown;
  if (!from_llm && !markdown) {
    response_goal = InferResponseGoalFromBlocksJson(raw_output);
  }

  ChatAnswerRml markdown_answer;
  std::string answer_text;
  ParseResult parsed;
  if (markdown) {
    // Cap the body first and append the link after: cutting the finished text at the store's limit
    // would drop exactly the trailing details link that the link table is rebuilt from.
    answer_text = WithDetailsLink(
        StructuredTextParser::StorableText(raw_output, kMaxComposeTextBytes - kDetailsLinkHeadroomBytes), sources,
        Tr("chat.view_details"));
    markdown_answer = BuildMarkdownAnswer(answer_text);
    parsed.ok = true;
    parsed.rml = markdown_answer.rml;
  } else {
    parsed = from_llm ? StructuredTextParser::ParseFromLlmOutput(raw_output, response_goal, render_mode)
                      : StructuredTextParser::ParseBlocksJson(raw_output, response_goal, render_mode);
  }
  if (!parsed.ok) {
    log().warning << "Failed to parse assistant reply: " << parsed.error;
    if (from_llm && !finish_reason.empty()) {
      log().warning << "LLM finish_reason: " << finish_reason;
    }
    if (host_.MessagingIsReady()) {
      const std::string active_thread = thread_id.empty() ? host_.ActiveThreadId() : thread_id;
      auto messages = host_.Facade()->GetMessagesPage(active_thread, std::nullopt, 10000);
      if (messages) {
        for (ThreadMessage& message : *messages) {
          if (message.id == entry_id) {
            message.content_rml = ApplyLangAttribute(R"(<div class="bubble bubble-assistant")", parsed.error) +
                                  R"( selectable="text"><p class="error">)" + StructuredTextParser::EscapeText(parsed.error) + "</p></div>";
            (void)host_.Facade()->UpdateMessage(message);
            break;
          }
        }
      }
    }
  } else {
    for (const std::string& warning : parsed.warnings) {
      log().warning << "Skipped block in assistant reply: " << warning;
    }

    if (!parsed.widget_inits.empty()) {
      widgets_.Initialize(entry_id, parsed.widget_inits);
    }

    std::vector<TranscriptChatAction> chat_actions;
    chat_actions.reserve(parsed.chat_actions.size());
    for (const ParsedChatAction& action : parsed.chat_actions) {
      chat_actions.push_back({action.label, action.message, action.payload});
    }

    // Working-set / bubble buttons must key send_chat_action to the ThreadMessage that
    // actually stores chat_actions — not a stale agent entry id when we append a new row.
    std::string action_entry_id = entry_id;
    const std::string active_thread = thread_id.empty() ? host_.ActiveThreadId() : thread_id;
    bool message_exists = false;
    if (host_.MessagingIsReady() && !action_entry_id.empty()) {
      auto messages = host_.Facade()->GetMessagesPage(active_thread, std::nullopt, 10000);
      if (messages) {
        for (const ThreadMessage& message : *messages) {
          if (message.id == action_entry_id) {
            message_exists = true;
            break;
          }
        }
      }
    }
    if (host_.MessagingIsReady() && !message_exists) {
      action_entry_id = util::GenerateUuid();
    }

    std::string hydrated = InjectEntryPlaceholders(parsed.rml, action_entry_id);
    std::vector<WorkingSetCandidate> working_set_candidates =
        working_set_.HydrateCandidates(parsed.working_set_candidates, action_entry_id);
    // Panel owns long_list row actions; only inline chips for chat-only replies.
    if (working_set_candidates.empty()) {
      hydrated = HydrateChatActionButtons(hydrated, chat_actions);
    }

    std::optional<std::string> working_set_json;
    if (!working_set_candidates.empty()) {
      working_set_json = WorkingSetCandidatesToJson(working_set_candidates);
    }

    const std::string assistant_open =
        ApplyLangAttribute(R"(<div class="bubble bubble-assistant")", raw_output) + R"( selectable="text">)";
    // B81: a blank first line shows in assistant bubbles; log how the body starts to see what sits before the text.
    log().debug << "assistant bubble markup head: " << hydrated.substr(0, 160);

    // A shared answer ("@ai+" / "@ai++") is stored once, by the relay below, as the "AI · shared" message
    // the peer also sees; a second local copy showed the same text twice (Kenneth 2026-10-28).
    const bool shared = shared_ai_mode == AtAiMode::SharedReply || shared_ai_mode == AtAiMode::SharedFull;
    if (host_.MessagingIsReady() && !shared) {
      if (message_exists) {
        auto messages = host_.Facade()->GetMessagesPage(active_thread, std::nullopt, 10000);
        if (messages) {
          for (ThreadMessage& message : *messages) {
            if (message.id == action_entry_id) {
              if (markdown) {
                // The session stored the raw answer; the details link must be in the stored text too,
                // or the link table cannot be rebuilt from it after a restart.
                message.text = StructuredTextParser::StorableText(answer_text);
              }
              message.content_rml = assistant_open + hydrated + "</div>";
              message.chat_actions = chat_actions;
              message.working_set_json = working_set_json;
              (void)host_.Facade()->UpdateMessage(message);
              break;
            }
          }
        }
      } else {
        ThreadMessage ai_message;
        ai_message.id = action_entry_id;
        ai_message.thread_id = active_thread;
        ai_message.sender_contact_id = kAiAssistantContactId;
        ai_message.text = StructuredTextParser::StorableText(markdown ? answer_text : raw_output);
        ai_message.content_rml = assistant_open + hydrated + "</div>";
        ai_message.chat_actions = chat_actions;
        ai_message.working_set_json = working_set_json;
        ai_message.timestamp = util::NowUnixMs();
        ai_message.transport = MessageTransport::Local;
        if (auto appended = host_.Facade()->AppendMessage(ai_message)) {
          if (!appended->id.empty()) {
            action_entry_id = appended->id;
            if (action_entry_id != ai_message.id) {
              hydrated = InjectEntryPlaceholders(parsed.rml, action_entry_id);
              working_set_candidates =
                  working_set_.HydrateCandidates(parsed.working_set_candidates, action_entry_id);
              if (working_set_candidates.empty()) {
                hydrated = HydrateChatActionButtons(hydrated, chat_actions);
              }
              if (!working_set_candidates.empty()) {
                working_set_json = WorkingSetCandidatesToJson(working_set_candidates);
              }
              ai_message.id = action_entry_id;
              ai_message.content_rml = assistant_open + hydrated + "</div>";
              ai_message.working_set_json = working_set_json;
              (void)host_.Facade()->UpdateMessage(ai_message);
            }
          }
        } else {
          log().warning << "Failed to append assistant message for chat actions: "
                        << appended.error().message;
        }
      }
      (void)host_.Facade()->UpdatePreview(active_thread, parsed.rml);
    }

    if (markdown) {
      chat_links_[action_entry_id] = std::move(markdown_answer.links);
    }

    working_set_.ApplyFromParse(action_entry_id, working_set_candidates, chat_actions);

    if (shared) {
      // The peer gets prose, never the blocks document (fenced answers used to go out as raw JSON).
      SendSharedAssistantRelay(active_thread, shared_ai_mode, StructuredTextParser::PlainText(raw_output));
    }
  }

  // An answer that arrived in a chat the user is not looking at shows as unread in the chat list (Kenneth
  // 2026-10-28: "在那个对话框列表那显示……一或者是二这样的一个提醒"). The open chat is never counted.
  if (!thread_id.empty() && host_.Facade() && !ChatPageShows(host_.ChromeSnapshot(), host_.ActiveThreadId(), thread_id)) {
    host_.Facade()->IncrementUnread(thread_id);
  }
  host_.SyncDisplayFromThread();
  host_.SyncShellSessions();
  chat_.status = "";
  if (!use_llm_) {
    MarkAgentIdle(); // the mock assistant has no session to say the turn ended
    DrainQueue();
  } else {
    RefreshBusyForActiveThread(); // the session's LoadingChanged(false) ends "busy"
  }
  DirtyChat();
  DirtyShell();
}

void AgentTurnController::HandleAgentEvent(const AgentEvent& event) {
  // An image turn rejected by the server (413 / 400) is worded here, not with the server's text.
  const char* image_error_key = AiImageErrorKindKey(event.error_kind);
  const std::string error_message = image_error_key ? Tr(image_error_key) : event.message;
  switch (event.type) {
  case AgentEventType::LoadingChanged:
    if (!event.loading && busy_.busy && !busy_.thread_id.empty() && !event.thread_id.empty() &&
        event.thread_id != busy_.thread_id) {
      // A late "ended" of an older turn: the queued turn that started after it is still running.
      log().debug << "Turn-end ignored: it belongs to an earlier turn";
      break;
    }
    if (!event.loading) {
      if (streaming_) { // turn ended without a persisted answer
        ClearStreamingRow();
        host_.SyncDisplayFromThread();
        DirtyChatTurns();
      }
      chat_.status = "";
      MarkAgentIdle();
      DrainQueue(); // the next thread's question, if one waits
    } else {
      MarkAgentBusy(event.thread_id);
      if (host_.MessagingIsReady()) {
        host_.SyncDisplayFromThread();
        DirtyChatTurns();
      }
    }
    DirtyChatChrome();
    break;
  case AgentEventType::ToolActivity:
    // The "answer" phase has no tool and gets no label of its own: the last step ("Searching the web…")
    // stays until the text arrives. Saying "Writing…" read as the AI making the answer up.
    if (event.tool_name.empty() && event.status == "answer") {
      break;
    }
    chat_.status = ui::String(ToolActivityLabel(event.tool_name, event.status).c_str());
    if (chat_.loading) { // the step label belongs to the thread whose turn runs
      host_.ShellSetActivity(true, chat_.status);
    }
    DirtyChatChrome();
    break;
  case AgentEventType::AssistantDelta:
    OnAssistantDelta(event);
    break;
  case AgentEventType::AssistantReady:
    ClearStreamingRow();
    FinishAssistantReply(event.entry_id, event.text, event.from_model,
                         event.finish_reason, event.thread_id, event.response_goal, event.render_mode,
                         event.shared_ai_mode, event.sources);
    break;
  case AgentEventType::ApprovalClosed:
    CloseApprovalCard(event.thread_id, event.entry_id, event.approval_outcome, event.approval_id);
    break;
  case AgentEventType::Error:
    log().error << "Agent session error: " << event.message;
    // A partial answer was already persisted via AssistantReady; the error toast is separate.
    UserFeedback::NeedsSetup(error_message);
    if (AgentReady() && agent_ports_.has_conversation_entries && agent_ports_.has_conversation_entries() &&
        agent_ports_.last_conversation_entry_id && agent_ports_.complete_assistant_message &&
        agent_ports_.set_assistant_display_plain) {
      if (auto entry_id = agent_ports_.last_conversation_entry_id()) {
        agent_ports_.complete_assistant_message(*entry_id, error_message);
        agent_ports_.set_assistant_display_plain(*entry_id, error_message);
      }
      host_.SyncDisplayFromThread();
      DirtyChatTurns();
    }
    chat_.status = "";
    // "Busy" ends with the turn's LoadingChanged(false), which follows; ending it here could mark the next queued
    // turn idle when that late event arrives.
    RefreshBusyForActiveThread();
    DirtyChatChrome();
    break;
  }
}

void AgentTurnController::RefreshLlmSetupBanner() {
  const AppConfig& config = host_.Store().Snapshot().config;
  use_llm_ = !config.llm.base_url.empty();
  const std::string kRegisterBriefHard = Tr("chat.hint.brief_register");
  const std::string kBriefUnavailable = Tr("chat.hint.brief_unavailable");
  const std::string kBriefUnavailableTemp = Tr("chat.hint.brief_unavailable_temp");

  if (!use_llm_) {
    UserFeedback::NeedsSetup(Tr("chat.hint.mock_replies"), /*sticky=*/true);
    return;
  }
  if (ResolvePreset(config) == "brief") {
    EnsureBriefGuestLlmKey();
    std::string registered;
    std::string guest;
    if (host_.MessagingReady()) {
      if (auto identity = host_.Facade()->GetIdentity()) {
        registered = identity->brief_llm_api_key;
        guest = identity->brief_llm_guest_api_key;
      }
    }
    const std::string brief_key = ResolveBriefLlmApiKey(registered, guest);
    if (brief_key.empty()) {
      // The mint hint can be the server's own wording, so remember what was shown to dismiss it later.
      last_brief_banner_ = !brief_guest_mint_user_hint_.empty() ? brief_guest_mint_user_hint_ : kBriefUnavailable;
      UserFeedback::NeedsSetup(last_brief_banner_, /*sticky=*/true);
      return;
    }
    const std::string& banner = host_.ChromeSnapshot().banner_message;
    if ((!last_brief_banner_.empty() && banner == last_brief_banner_) || banner == kRegisterBriefHard ||
        banner == kBriefUnavailable || banner == kBriefUnavailableTemp) {
      last_brief_banner_.clear();
      if (shell_feedback_.dismiss_banner) {
        shell_feedback_.dismiss_banner();
      }
    }
    return;
  }
  if (config.llm.require_api_key && config.llm.api_key.empty()) {
    UserFeedback::NeedsSetup(Tr("chat.hint.api_key_needed"), /*sticky=*/true);
  }
}

void AgentTurnController::EnsureBriefGuestLlmKey() {
  if (!host_.MessagingReady() || !host_.Facade()) {
    return;
  }
  auto identity = host_.Facade()->GetIdentity();
  if (!identity) {
    return;
  }
  if (!identity->brief_llm_api_key.empty() || !identity->brief_llm_guest_api_key.empty()) {
    brief_guest_mint_user_hint_.clear();
    return;
  }
  if (brief_guest_mint_attempted_) {
    return;
  }
  brief_guest_mint_attempted_ = true;

  std::string base_url = host_.Store().Snapshot().config.llm.base_url;
  if (ResolvePreset(host_.Store().Snapshot().config) != "brief" || base_url.empty()) {
    base_url = BriefLlmBaseUrl();
  }
  auto minted = MintBriefGuestLlmKey(base_url);
  if (!minted) {
    brief_guest_mint_user_hint_ = AppError::Display(minted.error());
    if (brief_guest_mint_user_hint_.empty()) {
      brief_guest_mint_user_hint_ = Tr("chat.hint.brief_unavailable_temp");
    }
    return;
  }
  brief_guest_mint_user_hint_.clear();
  LocalIdentity updated = *identity;
  updated.brief_llm_guest_api_key = minted->llm_api_key;
  if (!host_.Facade()->UpdateLocalIdentity(updated)) {
    brief_guest_mint_user_hint_ = Tr("chat.hint.brief_key_save_failed");
  }
}

AgentTurnController::AgentConfig AgentTurnController::ProjectAgent(const AppConfig& config) {
  return {.llm = config.llm,
          .llm_api_key_env = config.llm_api_key_env,
          .promoted_mcp = config.promoted_mcp,
          .mcp_servers = config.mcp_servers,
          .search = config.search,
          .context = config.context};
}

void AgentTurnController::ApplyConfig(const AgentConfig& config) {
  AgentConfig runtime = config;
  use_llm_ = !runtime.llm.base_url.empty();
  if (!AgentReady()) {
    return;
  }

  AppConfig preset_probe;
  preset_probe.llm = runtime.llm;
  if (ResolvePreset(preset_probe) == "brief") {
    runtime.llm.require_api_key = true;
    EnsureBriefGuestLlmKey();
    std::string brief_key;
    if (host_.MessagingInitialized() && host_.MessagingReady()) {
      if (auto identity = host_.Facade()->GetIdentity()) {
        brief_key = ResolveBriefLlmApiKey(identity->brief_llm_api_key, identity->brief_llm_guest_api_key);
      }
    }
    if (!brief_key.empty()) {
      runtime.llm.api_key = brief_key;
    } else {
      AppConfig last_probe;
      last_probe.llm = last_agent_runtime_.llm;
      if (ResolvePreset(last_probe) == "brief" && !last_agent_runtime_.llm.api_key.empty()) {
        // Me tab ReloadFromDisk may re-notify while identity is briefly unreadable.
        runtime.llm.api_key = last_agent_runtime_.llm.api_key;
      }
    }
  }

  if (!AgentConfigured() || runtime != last_agent_runtime_) {
    if (agent_ports_.set_tool_registration_hook) {
      agent_ports_.set_tool_registration_hook([this](ToolRegistry& tools) {
        if (host_.MessagingInitialized()) {
          host_.RegisterMessagingTools(tools);
        }
      });
    }
    AppConfig configure = host_.Store().IsInitialized()
                              ? host_.Store().Snapshot().config
                              : AppConfig{};
    configure.llm = runtime.llm;
    configure.llm_api_key_env = runtime.llm_api_key_env;
    configure.promoted_mcp = runtime.promoted_mcp;
    configure.mcp_servers = runtime.mcp_servers;
    configure.search = runtime.search;
    configure.context = runtime.context;
    if (agent_ports_.configure) {
      agent_ports_.configure(configure);
    }
    last_agent_runtime_ = std::move(runtime);
  }
}

void AgentTurnController::ReloadAgentConfig() {
  ApplyConfig(ProjectAgent(host_.Store().Snapshot().config));
}

void AgentTurnController::FlushHeldAgentSend() {
  if (!held_agent_send_.Holding()) {
    return;
  }
  HeldAgentSend::Send send;
  const HeldAgentSend::Step step =
      held_agent_send_.Poll(AgentCloudReady(), std::chrono::steady_clock::now(), send);
  if (step == HeldAgentSend::Step::Idle || step == HeldAgentSend::Step::Wait) {
    return;
  }
  if (step == HeldAgentSend::Step::Expired) {
    log().info << "Held send given up: the assistant did not connect in time";
    UserFeedback::NeedsSetup(Tr("chat.hint.assistant_not_connected"));
    return;
  }
  if (host_.ChromeSnapshot().banner_message == Tr("chat.status.connecting") && shell_feedback_.dismiss_banner) {
    shell_feedback_.dismiss_banner();
  }
  // Only a turn in the send's own thread holds it back; another thread's turn just queues it (SendUserText).
  switch (HeldSendFateOf(send, host_.ActiveThreadId(), busy_.BusyIn(send.thread_id))) {
  case HeldSendFate::Send:
    log().info << "Sending the held message: the assistant is connected";
    host_.SendUserText(send.text, std::move(send.payload), send.from_home_chip);
    return;
  case HeldSendFate::BackToDraft: {
    log().info << "Held send put back in its own composer: another thread is open, or a reply is loading";
    host_.PutBackDraft(send.thread_id, send.text);
    return;
  }
  case HeldSendFate::Drop:
    log().info << "Held chip send dropped: another thread is open, or a reply is loading";
    return;
  }
}

} // namespace pbr
