#include "gui/chat/PendingPictureRows.h"

#include "common/Utilities.h"
#include "gui/chat/TranscriptRml.h"

#include <ui/dom/Element.h>

#include <filesystem>
#include <system_error>

namespace pbr {

namespace {

void RemoveQuietly(const std::string& path) {
  std::error_code ec;
  std::filesystem::remove(path, ec);
}

} // namespace

PendingPictureRows::PendingPictureRows(RemoveFile remove_file)
    : remove_file_(remove_file ? std::move(remove_file) : RemoveFile(RemoveQuietly)) {}

bool PendingPictureRows::Apply(PendingPictureStep step) {
  if (!step.delete_file.empty()) {
    remove_file_(step.delete_file);
  }
  const PendingPictureUpload& next = step.next;
  const bool changed = next.state != upload_.state || next.row_id != upload_.row_id ||
                       next.thumb_path != upload_.thumb_path || next.pixels.width != upload_.pixels.width ||
                       next.pixels.height != upload_.pixels.height;
  upload_ = std::move(step.next);
  if (!upload_.Shows()) {
    row_.reset();
  }
  if (changed) {
    drawn_percent_ = upload_.percent; // drawn with the row again
  }
  return changed;
}

std::vector<MessageDisplayRow> PendingPictureRows::Render(std::vector<TranscriptRow> entries, TranscriptFacts facts,
                                                          const std::optional<MentionRoster>& mentions,
                                                          const std::string& thread_id, const bool at_tip) {
  const bool drawn_here = at_tip && upload_.ShowsIn(thread_id);
  if (drawn_here) {
    TranscriptRow pending;
    pending.message.id = upload_.row_id;
    pending.message.thread_id = thread_id;
    pending.message.sender_contact_id = kLocalSelfContactId;
    pending.message.content_type = ChatContentType::Attachment;
    pending.message.timestamp = util::NowUnixMs();
    pending.message.content_rml = PendingPictureRml(upload_);
    entries.push_back(std::move(pending));
    if (facts.send_mark) {
      // Never a send mark: nothing of it is on the wire yet (and its id is in no store).
      facts.send_mark = [mark = std::move(facts.send_mark), id = upload_.row_id](const ThreadMessage& message) {
        return message.id == id ? SendMark::None : mark(message);
      };
    }
  }
  std::vector<MessageDisplayRow> rows = RenderTranscript(entries, facts, mentions);
  if (drawn_here && !rows.empty() && rows.back().message_id == upload_.row_id.c_str()) {
    row_ = std::move(rows.back());
    rows.pop_back();
    drawn_percent_ = upload_.percent;
  } else if (upload_.ShowsIn(thread_id) || !upload_.Shows()) {
    row_.reset(); // not drawn in this window of its chat (older history shown): nothing to put back
  }
  return rows;
}

bool PendingPictureRows::KeepAtTail(std::vector<MessageDisplayRow>& messages, const std::string& thread_id,
                                    const bool at_tip) const {
  if (!row_ || !at_tip || !upload_.ShowsIn(thread_id) ||
      (!messages.empty() && messages.back().message_id == row_->message_id)) {
    return false;
  }
  messages.push_back(*row_);
  return true;
}

std::pair<std::string, size_t> PendingPictureRows::RealTail(const std::vector<MessageDisplayRow>& messages) {
  size_t count = messages.size();
  if (count > 0 && IsPendingUploadRowId(messages.back().message_id.c_str())) {
    --count;
  }
  return {count == 0 ? std::string() : std::string(messages[count - 1].message_id.c_str()), count};
}

bool PendingPictureRows::Progress(const int percent) {
  upload_ = OnPictureProgress(upload_, percent).next;
  if (!PendingProgressDue(drawn_percent_, upload_.percent)) {
    return false;
  }
  drawn_percent_ = upload_.percent;
  return true;
}

bool PendingPictureRows::DrawProgress(ui::Element* root, const int percent) {
  ui::Element* text = root ? root->GetElementById(kPendingPercentElementId) : nullptr;
  if (!text) {
    return false;
  }
  if (ui::Element* ring = root->GetElementById(kPendingRingElementId);
      ring && kPendingProgressRing && !ring->IsClassSet("chat-pending-ring--spin")) {
    ring->SetProperty("decorator", PendingRingDecorator(percent));
  }
  text->SetInnerRML(PendingPercentText(percent));
  return true;
}

void PendingPictureRows::DropThumbnail(ui::Element* root) {
  if (ui::Element* thumb = root ? root->GetElementById(kPendingThumbElementId) : nullptr) {
    if (ui::Element* parent = thumb->GetParentNode()) {
      (void)parent->RemoveChild(thumb);
    }
  }
}

} // namespace pbr
