#include "gui/chat/ChatMentionPicker.h"

#include "common/Utilities.h"
#include "foundation/i18n/LocalizationService.h"
#include "common/runtime/AppRuntime.h"
#include "lib/messaging/GroupRosterVersion.h"
#include "gui/shell/DataModelHost.h"
#include "lib/ui/ContextMenuHost.h"

#include <ui/base/StringUtilities.h>
#include <ui/dom/Context.h>
#include <ui/dom/Element.h>
#include <ui/dom/ElementDocument.h>
#include <ui/widgets/ElementFormControlTextArea.h>

#include <algorithm>
#include <utility>

namespace pbr {
namespace {

ui::ElementFormControlTextArea* DraftInput(ui::Context* context) {
  if (!context || context->GetNumDocuments() == 0) {
    return nullptr;
  }
  return ui_dynamic_cast<ui::ElementFormControlTextArea*>(context->GetDocument(0)->GetElementById("draft-input"));
}

} // namespace

ChatMentionPicker::ChatMentionPicker(ChatMenusHost& host, ChatPresenter& presenter, ChatScreenState& chat,
                                     ui::Context*& context)
    : host_(host),
      presenter_(presenter),
      chat_(chat),
      context_(context),
      // The group's lines are read on a worker when its chat opens; typing `@` and each keystroke after it only
      // filter what is held. The worker gets the facade by value (made here, on the UI thread, at each load) and
      // never the picker or the presenter; the AppRuntime teardown gate drains workers before the messaging graph
      // (and with it the facade) is torn down, and the reply is dropped by pp::Lifetime once the chat has changed.
      preload_(
          [](std::function<std::vector<MentionCandidate>()> read,
             std::function<void(std::vector<MentionCandidate>)> done) {
            AppRuntime::PostWorkerAndReplyOnUI<std::vector<MentionCandidate>>(WorkerLane::Normal, std::move(read),
                                                                              std::move(done));
          },
          [this]() -> MentionCandidatePreload::Reader {
            ConversationsFacade* facade = presenter_.Facade();
            return [facade](const std::string& thread_id) {
              return facade ? ChatPresenter::ReadMentionCandidates(facade, thread_id)
                            : std::vector<MentionCandidate>{};
            };
          },
          []() { return GroupRosterVersion::Get(); }, []() { return util::NowUnixMs(); }) {
  // Lines that landed: an open picker (or one waiting for its first lines) shows them.
  preload_.SetOnLoaded([this]() {
    if (at_ && (waiting_ || ContextMenuHost::Instance().IsOpen())) {
      waiting_ = false;
      RefreshLines();
    }
  });
}

void ChatMentionPicker::SyncThread() {
  const std::string thread_id = host_.ActiveThreadId();
  if (thread_id.empty() || !chat_.thread_is_group || !host_.MessagingIsReady()) {
    preload_.Drop();
    return;
  }
  if (thread_id != preload_.ThreadId()) {
    preload_.Drop();
    preload_.Open(thread_id);
  } else {
    preload_.Changed();  // messaging changed: a member joined or left, a name arrived
  }
}

size_t ChatMentionPicker::CaretByte(const std::string& text) const {
  ui::ElementFormControlTextArea* draft = DraftInput(context_);
  if (!draft) {
    return text.size();
  }
  int start = 0;
  int end = 0;
  draft->GetSelection(&start, &end, nullptr);
  // The input counts code points; the text is UTF-8 bytes (a Chinese draft).
  return static_cast<size_t>(ui::StringUtilities::ConvertCharacterOffsetToByteOffset(text, end));
}

std::string ChatMentionPicker::CurrentDraft() const {
  if (ui::ElementFormControlTextArea* draft = DraftInput(context_)) {
    return draft->GetValue().c_str();
  }
  return chat_.draft.c_str();
}

bool ChatMentionPicker::IsOpen() const {
  return at_.has_value() && ContextMenuHost::Instance().IsOpen();
}

void ChatMentionPicker::Close() {
  if (IsOpen()) {
    ContextMenuHost::Instance().Dismiss();
  }
  at_.reset();
  lines_.clear();
  waiting_ = false;
}

void ChatMentionPicker::OnDraftChanged() {
  const std::string thread_id = host_.ActiveThreadId();
  const std::string text = CurrentDraft();
  // Another chat's draft coming in is not something typed, and the picker was the other chat's.
  if (thread_id != last_thread_) {
    Close();
  }
  const std::string before = thread_id == last_thread_ ? last_text_ : text;
  // Picks whose token was deleted go now (not an empty draft: a refused send clears it, then restores it).
  if (thread_id == last_thread_ && !text.empty()) {
    if (const auto it = recorded_.find(thread_id); it != recorded_.end()) {
      PruneRecordedMentions(text, it->second);
    }
  }
  last_text_ = text;
  last_thread_ = thread_id;
  if (thread_id.empty() || !chat_.thread_is_group || !host_.MessagingIsReady()) {
    Close();
    return;
  }
  if (at_ && !waiting_ && !ContextMenuHost::Instance().IsOpen()) {
    at_.reset();  // closed from outside (Escape, a tap beside it): typing on does not bring it back
    lines_.clear();
  }
  if (!at_) {
    const std::optional<size_t> trigger = TypedMentionTrigger(before, text);
    if (!trigger) {
      return;
    }
    at_ = *trigger;
    if (thread_id != preload_.ThreadId()) {
      SyncThread();  // the chat opened without the picker hearing of it
    }
    preload_.Typed();
  }
  if (!preload_.Ready()) {
    // The group's lines are still being read (off the UI thread): the picker opens when they land.
    waiting_ = true;
    return;
  }
  RefreshLines();
}

void ChatMentionPicker::RefreshLines() {
  if (!at_) {
    return;
  }
  const std::string text = CurrentDraft();
  caret_ = CaretByte(text);
  const std::optional<std::string> query = MentionQueryAt(text, *at_, caret_);
  std::vector<MentionCandidate> lines = query ? preload_.Filter(*query) : std::vector<MentionCandidate>{};
  if (lines.empty()) {
    Close();
    return;
  }
  if (lines == lines_ && ContextMenuHost::Instance().IsOpen()) {
    return;  // the same lines are showing
  }
  Show(lines);
}

void ChatMentionPicker::Show(const std::vector<MentionCandidate>& lines) {
  lines_ = lines;
  std::vector<ContextMenuAction> actions;
  actions.reserve(lines.size());
  for (size_t i = 0; i < lines.size(); ++i) {
    const MentionCandidate& line = lines[i];
    const bool everyone = line.kind == TextSpanKind::Everyone;
    actions.push_back({"mention_" + std::to_string(i), everyone ? Tr("chat.mention.everyone") : line.Label(), nullptr,
                       [this, line]() { Choose(line); }, everyone ? "../icons/group.svg" : "../icons/contacts.svg"});
  }
  // Over the composer, so the list does not hide what is being typed.
  ui::Vector2i position(120, 120);
  if (ui::ElementFormControlTextArea* draft = DraftInput(context_)) {
    const ui::Vector2f offset = draft->GetAbsoluteOffset(ui::BoxArea::Border);
    position = ui::Vector2i(static_cast<int>(offset.x), static_cast<int>(offset.y) - 4);
  }
  ContextMenuHost::Instance().ShowActionsAbove(position, std::move(actions));
}

void ChatMentionPicker::Choose(const MentionCandidate& line) {
  if (!at_) {
    return;
  }
  const std::string thread_id = host_.ActiveThreadId();
  const std::string draft_text = CurrentDraft();
  const MentionInsert inserted = InsertMention(draft_text, *at_, caret_, line.Token());
  // An earlier pick of this name whose token was deleted must not outlive it (the `@query` being replaced is not one).
  const std::string kept_text = draft_text.substr(0, *at_) + draft_text.substr(std::min(caret_, draft_text.size()));
  std::vector<ComposerMention>& picks = recorded_[thread_id];
  PruneRecordedMentions(kept_text, picks);
  at_.reset();
  lines_.clear();
  picks.push_back({line.kind, line.account_id, line.Token()});
  last_text_ = inserted.text;  // the change this makes is ours, not typing
  last_thread_ = thread_id;
  chat_.draft = inserted.text.c_str();
  DataModelHost::Instance().Dirty("chat", "draft");
  ui::ElementFormControlTextArea* draft = DraftInput(context_);
  if (!draft) {
    return;
  }
  draft->SetValue(inserted.text.c_str());
  const ui::String value = draft->GetValue();
  const int caret = ui::StringUtilities::ConvertByteOffsetToCharacterOffset(value, static_cast<int>(inserted.caret));
  draft->Focus();
  draft->SetSelectionRange(caret, caret);
}

bool ChatMentionPicker::ChooseFirst() {
  if (!IsOpen() || lines_.empty()) {
    return false;
  }
  const MentionCandidate first = lines_.front();
  ContextMenuHost::Instance().Dismiss();
  Choose(first);
  return true;
}

std::vector<TextSpan> ChatMentionPicker::SpansFor(const std::string& thread_id, const std::string& text) const {
  const auto it = recorded_.find(thread_id);
  return it == recorded_.end() ? std::vector<TextSpan>{} : MentionSpansForDraft(text, it->second);
}

void ChatMentionPicker::ForgetThread(const std::string& thread_id) {
  recorded_.erase(thread_id);
  if (thread_id == last_thread_) {
    // Sent (or gone): the draft was cleared in code, with no draft_changed — what was typed before is not the
    // text the next `@` is typed into (it would not be seen as typed, and the picker would not open).
    last_text_.clear();
  }
}

void ChatMentionPicker::Reset() {
  Close();
  preload_.Drop();
  recorded_.clear();
  last_text_.clear();
  last_thread_.clear();
}

} // namespace pbr
