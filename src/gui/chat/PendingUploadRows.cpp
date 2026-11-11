#include "gui/chat/PendingUploadRows.h"

#include "common/Utilities.h"
#include "gui/chat/TranscriptRml.h"

#include <ui/dom/Element.h>

#include <algorithm>
#include <filesystem>
#include <system_error>

namespace pbr {

namespace {

void RemoveQuietly(const std::string& path) {
  std::error_code ec;
  std::filesystem::remove(path, ec);
}

} // namespace

PendingUploadRows::PendingUploadRows(RemoveFile remove_file)
    : remove_file_(remove_file ? std::move(remove_file) : RemoveFile(RemoveQuietly)) {}

const PendingUpload& PendingUploadRows::Upload() const {
  static const PendingUpload kNone;
  return queue_.Empty() ? kNone : queue_.Items().front();
}

std::optional<MessageDisplayRow> PendingUploadRows::Row() const {
  return rows_.empty() ? std::nullopt : std::optional<MessageDisplayRow>(rows_.front());
}

bool PendingUploadRows::Apply(PendingUploadStep step) {
  if (step.next.Shows() && !step.next.row_id.empty() && !queue_.Find(step.next.row_id)) {
    if (!step.delete_file.empty()) {
      remove_file_(step.delete_file);
    }
    const std::string id = step.next.row_id;
    queue_.Add(std::move(step.next));
    drawn_percent_[id] = queue_.Find(id)->percent;
    return true;
  }
  if (queue_.Empty()) {
    if (!step.delete_file.empty()) {
      remove_file_(step.delete_file); // a thumbnail for a row that is gone
    }
    return false;
  }
  return Apply(Upload().row_id, [&step](const PendingUpload&) { return step; });
}

bool PendingUploadRows::Apply(const std::string& id, const Event& event) {
  const PendingUpload* found = queue_.Find(id);
  if (!found) {
    return false;
  }
  const std::optional<PendingUpload> before = *found;
  return AfterChange(id, before, queue_.Apply(id, event));
}

bool PendingUploadRows::AfterChange(const std::string& id, const std::optional<PendingUpload>& before,
                                    std::vector<std::string> files) {
  for (const std::string& file : files) {
    remove_file_(file);
  }
  const PendingUpload* now = queue_.Find(id);
  auto row = std::find_if(rows_.begin(), rows_.end(),
                          [&id](const MessageDisplayRow& kept) { return kept.message_id == id.c_str(); });
  if (!now) {
    if (row != rows_.end()) {
      rows_.erase(row);
    }
    drawn_percent_.erase(id);
    return before.has_value();
  }
  const bool changed = !before || now->state != before->state || now->thumb_path != before->thumb_path ||
                       now->pixels.width != before->pixels.width || now->pixels.height != before->pixels.height ||
                       now->title != before->title || now->size_text != before->size_text;
  if (before && !before->thumb_path.empty() && now->thumb_path.empty() && row != rows_.end()) {
    // The picture out of the kept row (PictureClipRml writes no <img> without a thumbnail): the grey placeholder.
    std::string rml = row->content_rml.c_str();
    const size_t id_at = rml.find("id=\"" + PendingThumbElementId(id) + "\"");
    const size_t start = id_at == std::string::npos ? id_at : rml.rfind("<img ", id_at);
    const size_t end = start == std::string::npos ? start : rml.find("/>", id_at);
    if (end != std::string::npos) {
      rml.erase(start, end + 2 - start);
      row->content_rml = rml.c_str();
    } else {
      rows_.erase(row); // not found: never put back a stale picture; the next drawing makes the row again
    }
  }
  if (changed) {
    drawn_percent_[id] = now->percent; // drawn with the row again
  }
  return changed;
}

std::vector<MessageDisplayRow> PendingUploadRows::Render(std::vector<TranscriptRow> entries, TranscriptFacts facts,
                                                         const std::optional<MentionRoster>& mentions,
                                                         const std::string& thread_id, const bool at_tip) {
  std::vector<std::string> drawn_ids;
  if (at_tip) {
    for (const PendingUpload& item : queue_.Items()) {
      if (!item.ShowsIn(thread_id)) {
        continue;
      }
      TranscriptRow pending;
      pending.message.id = item.row_id;
      pending.message.thread_id = thread_id;
      pending.message.sender_contact_id = kLocalSelfContactId;
      pending.message.content_type = ChatContentType::Attachment;
      pending.message.timestamp = util::NowUnixMs();
      pending.message.content_rml = PendingUploadRml(item);
      entries.push_back(std::move(pending));
      drawn_ids.push_back(item.row_id);
    }
  }
  if (!drawn_ids.empty() && facts.send_mark) {
    // Never a send mark: nothing of them is on the wire yet (and their ids are in no store).
    facts.send_mark = [mark = std::move(facts.send_mark)](const ThreadMessage& message) {
      return IsPendingUploadRowId(message.id) ? SendMark::None : mark(message);
    };
  }
  std::vector<MessageDisplayRow> rows = RenderTranscript(entries, facts, mentions);
  if (!drawn_ids.empty()) {
    std::vector<MessageDisplayRow> kept;
    while (!rows.empty() && IsPendingUploadRowId(rows.back().message_id.c_str())) {
      kept.insert(kept.begin(), std::move(rows.back()));
      rows.pop_back();
    }
    rows_ = std::move(kept);
    for (const std::string& id : drawn_ids) {
      drawn_percent_[id] = queue_.Find(id)->percent;
    }
  } else if (queue_.ShowsIn(thread_id) || queue_.Empty()) {
    rows_.clear(); // not drawn in this window of their chat (older history shown): nothing to put back
  }
  return rows;
}

void PendingUploadRows::Drop() {
  for (const std::string& file : queue_.Clear()) {
    remove_file_(file);
  }
  rows_.clear();
  drawn_percent_.clear();
}

bool PendingUploadRows::RefreshTail(std::vector<MessageDisplayRow>& messages) const {
  bool changed = false;
  for (auto it = messages.rbegin(); it != messages.rend() && IsPendingUploadRowId(it->message_id.c_str()); ++it) {
    for (const MessageDisplayRow& kept : rows_) {
      if (kept.message_id == it->message_id && kept.content_rml != it->content_rml) {
        it->content_rml = kept.content_rml;
        changed = true;
      }
    }
  }
  return changed;
}

bool PendingUploadRows::KeepAtTail(std::vector<MessageDisplayRow>& messages, const std::string& thread_id,
                                   const bool at_tip) const {
  if (rows_.empty() || !at_tip || !queue_.ShowsIn(thread_id)) {
    return false;
  }
  const size_t real = RealTail(messages).second;
  bool same = messages.size() - real == rows_.size();
  for (size_t i = 0; same && i < rows_.size(); ++i) {
    same = messages[real + i].message_id == rows_[i].message_id;
  }
  if (same) {
    return false;
  }
  messages.resize(real);
  messages.insert(messages.end(), rows_.begin(), rows_.end());
  return true;
}

std::pair<std::string, size_t> PendingUploadRows::RealTail(const std::vector<MessageDisplayRow>& messages) {
  size_t count = messages.size();
  while (count > 0 && IsPendingUploadRowId(messages[count - 1].message_id.c_str())) {
    --count;
  }
  return {count == 0 ? std::string() : std::string(messages[count - 1].message_id.c_str()), count};
}

std::optional<std::string> PendingUploadRows::Progress(const int percent) {
  for (const PendingUpload& item : queue_.Items()) {
    if (item.state != PendingUploadState::Uploading) {
      continue;
    }
    const std::string id = item.row_id;
    (void)queue_.Apply(id, [percent](const PendingUpload& now) { return OnUploadProgress(now, percent); });
    const int shown = queue_.Find(id)->percent;
    int& drawn = drawn_percent_[id];
    if (!PendingProgressDue(drawn, shown)) {
      return std::nullopt;
    }
    drawn = shown;
    return id;
  }
  return std::nullopt;
}

bool PendingUploadRows::DrawProgress(ui::Element* root, const std::string& id, const int percent) {
  ui::Element* text = root ? root->GetElementById(PendingPercentElementId(id)) : nullptr;
  if (!text) {
    return false;
  }
  if (ui::Element* ring = root->GetElementById(PendingRingElementId(id));
      ring && kPendingProgressRing && !ring->IsClassSet("chat-pending-ring--spin")) {
    ring->SetProperty("decorator", PendingRingDecorator(percent));
  }
  text->SetInnerRML(PendingPercentText(percent));
  return true;
}

void PendingUploadRows::DropThumbnail(ui::Element* root, const std::string& id) {
  if (ui::Element* thumb = root ? root->GetElementById(PendingThumbElementId(id)) : nullptr) {
    if (ui::Element* parent = thumb->GetParentNode()) {
      (void)parent->RemoveChild(thumb);
    }
  }
}

} // namespace pbr
