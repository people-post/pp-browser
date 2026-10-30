#include "gui/chat/ChatMentionController.h"

#include "feature/conversations/ChatPresenter.h"
#include "foundation/i18n/LocalizationService.h"
#include "gui/shell/DataModelHost.h"
#include "lib/messaging/GroupMentions.h"

#include <chrono>

#include <ui/dom/Context.h>
#include <ui/dom/Element.h>
#include <ui/dom/ElementDocument.h>
#include <ui/dom/Event.h>
#include <ui/widgets/ElementFormControlTextArea.h>

namespace pbr {

namespace {

constexpr const char* kDraftId = "draft-input";
constexpr const char* kPopupId = "mention-popup";

bool InsidePopup(ui::Element* element) {
  for (; element; element = element->GetParentNode()) {
    if (element->GetId() == kPopupId) {
      return true;
    }
  }
  return false;
}

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

} // namespace

ChatMentionController::ChatMentionController(ChatScreenState& state, ChatPresenter& presenter,
                                             ui::Context*& context, Ports ports)
    : state_(state), presenter_(presenter), context_(context), ports_(std::move(ports)) {}

ChatMentionController::~ChatMentionController() {
  Detach();
}

ui::ElementFormControlTextArea* ChatMentionController::DraftBox() const {
  if (!context_ || context_->GetNumDocuments() == 0) {
    return nullptr;
  }
  return ui_dynamic_cast<ui::ElementFormControlTextArea*>(context_->GetDocument(0)->GetElementById(kDraftId));
}

void ChatMentionController::Attach(ui::Element* document) {
  if (document == attached_document_) {
    return;
  }
  Detach(); // from the old document, if it is still around
  attached_document_ = document;
  if (document) {
    document->AddEventListener(ui::EventId::Textinput, this, true);
    document->AddEventListener(ui::EventId::Mousedown, this, true);
  }
}

void ChatMentionController::Detach() {
  ui::Element* document = attached_document_;
  attached_document_ = nullptr;
  if (document) {
    document->RemoveEventListener(ui::EventId::Textinput, this, true);
    document->RemoveEventListener(ui::EventId::Mousedown, this, true);
  }
}

void ChatMentionController::OnDetach(ui::Element* element) {
  // Fired when the document is destroyed (a reload at the same address included) and by our own Remove calls.
  if (element == attached_document_) {
    attached_document_ = nullptr;
  }
}

void ChatMentionController::ProcessEvent(ui::Event& event) {
  ui::Element* target = event.GetTargetElement();
  if (event.GetId() == ui::EventId::Textinput) {
    if (target && target->GetId() == kDraftId) {
      committed_input_ = true;
    }
  } else if (event.GetId() == ui::EventId::Mousedown) {
    // A tap outside the popup and the box closes the popup.
    if (draft_.PopupOpen() && target && target->GetId() != kDraftId && !InsidePopup(target)) {
      draft_.ClosePopup();
      SyncState();
    }
  }
}

void ChatMentionController::Reconfigure(const std::string& thread_id) {
  thread_id_ = thread_id;
  const MentionTargets targets = presenter_.MentionTargetsOf(thread_id);
  targets_retry_.Record(targets.unknown, NowMs());
  if (!targets.is_group) {
    draft_.Disable();
    return;
  }
  // The members are read when the popup opens, so a roster change shows up the next time.
  draft_.Enable(
      [this, thread_id] {
        const MentionTargets now = presenter_.MentionTargetsOf(thread_id);
        draft_.SetOwner(now.is_owner);
        return now.members;
      },
      targets.is_owner, Tr("chat.mention.everyone"));
  draft_.Reset();
}

void ChatMentionController::Tick() {
  if (!context_ || context_->GetNumDocuments() == 0) {
    return;
  }
  Attach(context_->GetDocument(0));
  const std::string thread = ports_.thread_id ? ports_.thread_id() : std::string();
  if (thread != thread_id_ || targets_retry_.Due(NowMs())) {
    Reconfigure(thread);
    SyncState();
  }
  if (!draft_.Enabled()) {
    committed_input_ = false;
    return;
  }
  ui::ElementFormControlTextArea* box = DraftBox();
  const bool typed = committed_input_;
  committed_input_ = false;
  if (!box || context_->GetFocusElement() != box) {
    if (draft_.PopupOpen()) {
      draft_.ClosePopup(); // the box lost focus (tapping the popup keeps it)
      SyncState();
    }
    return;
  }
  const std::string text = box->GetValue();
  int sel_start = 0;
  int sel_end = 0;
  box->GetSelection(&sel_start, &sel_end, nullptr);
  if (sel_start != sel_end) {
    draft_.ClosePopup();
    SyncState();
    return;
  }
  if (const auto edit = draft_.OnDraftChanged(text, sel_end, typed)) {
    Apply(*edit);
  }
  SyncState();
}

void ChatMentionController::Apply(const DraftEdit& edit) {
  if (ui::ElementFormControlTextArea* box = DraftBox()) {
    box->SetValue(edit.text.c_str());
    box->SetSelectionRange(edit.cursor_chars, edit.cursor_chars);
  }
  state_.draft = edit.text.c_str();
  DataModelHost::Instance().Dirty("chat", "draft");
}

void ChatMentionController::Pick(const int row_index) {
  const ui::ElementFormControlTextArea* box = DraftBox();
  if (const auto edit = draft_.Pick(row_index, box ? box->GetValue() : std::string())) {
    Apply(*edit);
  }
  SyncState();
}

void ChatMentionController::SyncState() {
  const bool open = draft_.PopupOpen();
  const bool empty = draft_.PopupEmpty();
  std::vector<MentionPopupRow> rows;
  if (open) {
    for (const MentionRow& row : draft_.Rows()) {
      rows.push_back({row.index, row.label.c_str(), row.everyone});
    }
  }
  const auto same_rows = [&] {
    if (rows.size() != state_.mention_rows.size()) {
      return false;
    }
    for (size_t i = 0; i < rows.size(); ++i) {
      if (rows[i].index != state_.mention_rows[i].index || rows[i].label != state_.mention_rows[i].label ||
          rows[i].everyone != state_.mention_rows[i].everyone) {
        return false;
      }
    }
    return true;
  };
  if (open == state_.mention_open && empty == state_.mention_empty && same_rows()) {
    return;
  }
  state_.mention_open = open;
  state_.mention_empty = empty;
  state_.mention_rows = std::move(rows);
  state_.mention_empty_text = open ? Tr("chat.mention.no_match").c_str() : "";
  DataModelHost::Instance().Dirty("chat", "mention_open");
  DataModelHost::Instance().Dirty("chat", "mention_empty");
  DataModelHost::Instance().Dirty("chat", "mention_rows");
  DataModelHost::Instance().Dirty("chat", "mention_empty_text");
}

std::optional<std::string> ChatMentionController::PayloadForSend(const std::string& draft_text,
                                                                 const std::string& sent_text) const {
  const std::vector<DraftMention> kept = draft_.FinalizeForSend(draft_text, sent_text);
  if (kept.empty()) {
    return std::nullopt;
  }
  MentionsPayload payload;
  for (const DraftMention& mention : kept) {
    const MentionRange range{mention.account_id, static_cast<uint32_t>(mention.start),
                             static_cast<uint32_t>(mention.end)};
    if (mention.all) {
      payload.all = range;
    } else {
      payload.mentions.push_back(range);
    }
  }
  return BuildMentionsPayloadJson(sent_text, payload);
}

} // namespace pbr
