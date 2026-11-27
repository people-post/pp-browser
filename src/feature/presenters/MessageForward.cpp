#include "feature/presenters/MessageForward.h"

#include "feature/presenters/PictureSendQueue.h"
#include "lib/delivery/ChatPayloadCodec.h"
#include "lib/messaging/AttachmentCache.h"
#include "lib/voice/VoiceFile.h"

#include <algorithm>
#include <cctype>
#include <iterator>
#include <set>
#include <utility>
#include "common/PbrCompat.h"

namespace pbr {

bool CanForwardMessage(const ThreadMessage& message) {
  if (IsLocalActionMessage(message) || message.ai_image_json) {
    return false;
  }
  // Our own text and cards forward whatever their delivery state: the words are settled the moment they are sent.
  switch (message.content_type) {
    case ChatContentType::Text:
      return !message.text.empty();
    case ChatContentType::ContactCard:
      // Sent again as stored: the card must still read as one (nothing is rebuilt from the contact it names).
      return static_cast<bool>(ChatPayloadCodec::DecodeContactCardJson(message.payload_json));
    case ChatContentType::Attachment: {
      // Resent from the stored fields and bytes: it needs a name for the file and a hash to find the bytes by.
      auto fields = ChatPayloadCodec::DecodeAttachmentJson(message.payload_json);
      return fields && !fields->content_hash.empty();
    }
    case ChatContentType::System:
    case ChatContentType::Annotation:
    case ChatContentType::CryptoTx:
    case ChatContentType::Unsupported:
    case ChatContentType::Deleted:
      break;
  }
  return false;
}

ForwardItemKind ForwardKindOf(const ThreadMessage& message) {
  if (message.content_type == ChatContentType::ContactCard) {
    return ForwardItemKind::Card;
  }
  if (message.content_type != ChatContentType::Attachment) {
    return ForwardItemKind::Text;
  }
  auto fields = ChatPayloadCodec::DecodeAttachmentJson(message.payload_json);
  if (!fields) {
    return ForwardItemKind::File;
  }
  if (fields->mime == kVoiceMime && ParseVoiceFileName(fields->filename) &&
      fields->byte_length <= kVoiceBubbleMaxBytes) {
    return ForwardItemKind::Voice;
  }
  if (IsAttachmentImageMime(fields->mime)) {
    return ForwardItemKind::Picture;
  }
  if (fields->mime.rfind("video/", 0) == 0) {
    return ForwardItemKind::Video;
  }
  return ForwardItemKind::File;
}

ForwardCheck CheckForwardLimits(std::vector<ForwardItemFacts> items, const size_t targets, const bool attachment_busy) {
  ForwardCheck check;
  for (const ForwardItemFacts& item : items) {
    if (item.kind == ForwardItemKind::Text || item.kind == ForwardItemKind::Card) {
      continue; // never counted
    }
    ++check.attachments;
    if (!item.has_local_bytes) {
      ++check.without_local_bytes; // it will not be sent, so it is in no count
      continue;
    }
    ++check.uploads;
    check.upload_bytes += item.byte_length;
    check.largest_bytes = std::max(check.largest_bytes, item.byte_length);
  }
  check.uploads *= targets;
  check.upload_bytes *= targets;
  check.items = std::move(items);
  if (check.uploads == 0 && check.attachments == 0) {
    return check; // nothing but text and cards: the limits and the busy attachment slot do not matter
  }
  if (attachment_busy && check.uploads > 0) {
    check.refusal = ForwardRefusal::AttachmentBusy;
  } else if (check.largest_bytes > kMaxForwardUploadBytes) {
    check.refusal = ForwardRefusal::SingleTooLarge;
  } else if (check.uploads > kMaxForwardUploads) {
    check.refusal = ForwardRefusal::TooManyUploads;
  } else if (check.upload_bytes > kMaxForwardUploadBytes) {
    check.refusal = ForwardRefusal::TooLarge;
  }
  return check;
}

std::string ForwardPreviewText(const std::vector<ThreadMessage>& messages,
                               const std::function<std::string(size_t count)>& count_text) {
  if (messages.empty()) {
    return {};
  }
  if (messages.size() > 1) {
    return count_text ? count_text(messages.size()) : std::to_string(messages.size());
  }
  const ThreadMessage& message = messages.front();
  if (message.content_type == ChatContentType::ContactCard) {
    if (auto card = ChatPayloadCodec::DecodeContactCardJson(message.payload_json);
        card && !card->display_name.empty()) {
      return card->display_name;
    }
    return message.text;
  }
  // The first two lines; the view clips what is longer than the strip.
  std::string out;
  size_t from = 0;
  for (int line = 0; line < 2 && from <= message.text.size(); ++line) {
    const size_t end = message.text.find('\n', from);
    const std::string part = message.text.substr(from, end == std::string::npos ? std::string::npos : end - from);
    out += (line == 0 ? "" : "\n") + part;
    if (end == std::string::npos) {
      break;
    }
    from = end + 1;
  }
  return out;
}

ForwardTargetKind ClassifyForwardTarget(const Thread& thread, const ForwardTargetFacts& facts) {
  if (thread.kind == ThreadKind::Group) {
    return thread.group_id && facts.in_group && !facts.group_dissolved ? ForwardTargetKind::Group
                                                                       : ForwardTargetKind::Unavailable;
  }
  if (thread.kind != ThreadKind::Direct) {
    return ForwardTargetKind::Unavailable; // an AI conversation
  }
  if (!facts.saved_contact || facts.blocked) {
    return ForwardTargetKind::Unavailable;
  }
  const std::string& peer = thread.peer_identity_value;
  if (!peer.empty() && (peer == facts.my_account || peer == facts.support_account)) {
    return ForwardTargetKind::Unavailable; // ourselves, or the PP support chat
  }
  return ForwardTargetKind::Direct;
}

namespace {

bool NameBefore(const ForwardTarget& a, const ForwardTarget& b) {
  const auto fold = [](std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
  };
  const std::string left = fold(a.title);
  const std::string right = fold(b.title);
  return left != right ? left < right : a.Key() < b.Key();
}

} // namespace

std::vector<ForwardTarget> OrderForwardTargets(std::vector<ForwardTarget> targets) {
  // Once each: a chat by its id, a person with a chat by that chat.
  std::set<std::string> seen;
  std::set<std::string> contacts_with_a_chat;
  std::vector<ForwardTarget> unique;
  for (const ForwardTarget& target : targets) {
    if (!target.thread_id.empty() && !target.contact_id.empty()) {
      contacts_with_a_chat.insert(target.contact_id);
    }
  }
  for (ForwardTarget& target : targets) {
    if (target.thread_id.empty() && contacts_with_a_chat.count(target.contact_id) > 0) {
      continue;
    }
    if (seen.insert(target.Key()).second) {
      unique.push_back(std::move(target));
    }
  }
  std::vector<ForwardTarget> chats;
  for (const ForwardTarget& target : unique) {
    if (!target.thread_id.empty() && target.updated_at > 0) {
      chats.push_back(target);
    }
  }
  std::stable_sort(chats.begin(), chats.end(), [](const ForwardTarget& a, const ForwardTarget& b) {
    return a.updated_at != b.updated_at ? a.updated_at > b.updated_at : NameBefore(a, b);
  });
  if (chats.size() > kForwardRecentChats) {
    chats.resize(kForwardRecentChats);
  }
  std::set<std::string> recent;
  for (const ForwardTarget& target : chats) {
    recent.insert(target.Key());
  }
  std::vector<ForwardTarget> people;
  std::vector<ForwardTarget> groups;
  for (ForwardTarget& target : unique) {
    if (recent.count(target.Key()) > 0) {
      continue;
    }
    (target.group ? groups : people).push_back(std::move(target));
  }
  std::sort(people.begin(), people.end(), NameBefore);
  std::sort(groups.begin(), groups.end(), NameBefore);
  chats.insert(chats.end(), std::make_move_iterator(people.begin()), std::make_move_iterator(people.end()));
  chats.insert(chats.end(), std::make_move_iterator(groups.begin()), std::make_move_iterator(groups.end()));
  return chats;
}

/** One attachment send's answer, shared by the run and the callback it handed to the messaging layer. */
struct ForwardRun::Completion {
  std::optional<Roe<ThreadMessage>> answer;
  std::function<void()> wake;
  std::atomic<uint64_t> bytes_done{0};
  std::atomic<uint64_t> bytes_total{0};
};

ForwardRun::ForwardRun(ForwardPorts ports, std::string source_thread, std::vector<std::string> message_ids,
                       std::vector<ForwardTarget> targets)
    : ports_(std::move(ports)), source_thread_(std::move(source_thread)), message_ids_(std::move(message_ids)) {
  message_count_ = message_ids_.size();
  for (ForwardTarget& target : targets) {
    const auto same = [&target](const ForwardTarget& kept) { return kept.Key() == target.Key(); };
    if (std::find_if(targets_.begin(), targets_.end(), same) == targets_.end()) {
      targets_.push_back(std::move(target));
    }
  }
  result_.total_items = message_count_ * targets_.size();
}

void ForwardRun::SetWake(std::function<void()> wake) {
  wake_ = std::move(wake);
}

bool ForwardRun::Prepare() {
  prepared_ = true;
  for (const std::string& id : message_ids_) {
    std::optional<ThreadMessage> row = ports_.get_message ? ports_.get_message(source_thread_, id) : std::nullopt;
    if (!row || !CanForwardMessage(*row)) {
      ++result_.skipped_messages;
      continue;
    }
    messages_.push_back(std::move(*row));
  }
  std::stable_sort(messages_.begin(), messages_.end(), [](const ThreadMessage& a, const ThreadMessage& b) {
    return a.display_order != b.display_order ? a.display_order < b.display_order : a.timestamp < b.timestamp;
  });
  send_failed_.assign(messages_.size(), false);
  fields_.assign(messages_.size(), std::nullopt);
  for (size_t i = 0; i < messages_.size(); ++i) {
    if (messages_[i].content_type == ChatContentType::Attachment) {
      if (auto fields = ChatPayloadCodec::DecodeAttachmentJson(messages_[i].payload_json)) {
        fields_[i] = std::move(*fields);
        has_attachments_ = true;
        result_.attachments += targets_.size();
      }
    }
  }
  return !messages_.empty();
}

bool ForwardRun::BeginTarget() {
  const ForwardTarget& target = targets_[target_at_];
  target_thread_ = target.thread_id;
  if (target_thread_.empty() && !target.contact_id.empty() &&
      (!ports_.contact_can_receive || ports_.contact_can_receive(target.contact_id)) && ports_.direct_thread_of) {
    // A person with no chat yet: the chat is made now, after the re-check says they may still be written to, and each
    // person whose chat cannot be made is a chat not reached.
    target_thread_ = ports_.direct_thread_of(target.contact_id);
  }
  target_kind_ =
      !target_thread_.empty() && ports_.target_of ? ports_.target_of(target_thread_) : ForwardTargetKind::Unavailable;
  if (target_kind_ == ForwardTargetKind::Unavailable) {
    ++result_.skipped_targets;
    ++target_at_;
    return false;
  }
  target_open_ = true;
  message_at_ = 0;
  last_sent_id_.clear();
  last_was_attachment_ = false;
  return true;
}

void ForwardRun::FailMessage(const size_t message_at) {
  if (fields_[message_at]) {
    ++result_.attachments_failed;
  }
  if (!send_failed_[message_at]) {
    // Counted once per message, however many chats refused it.
    send_failed_[message_at] = true;
    ++result_.send_errors;
  }
}

void ForwardRun::FailRestOfTarget() {
  for (size_t i = message_at_; i < messages_.size(); ++i) {
    FailMessage(i);
  }
  ++result_.skipped_targets;
  target_open_ = false;
  ++target_at_;
  last_sent_id_.clear();
  last_was_attachment_ = false;
}

void ForwardRun::NextItem() {
  if (++message_at_ >= messages_.size()) {
    target_open_ = false;
    ++target_at_;
    last_sent_id_.clear();
    last_was_attachment_ = false;
  }
}

bool ForwardRun::Settled(const std::string& thread_id, const std::string& message_id) {
  // The row as stored now: still "queued" means the messaging layer has not yet sent it on or given up (a later send of
  // this chat may overtake it in a group, or when the peer is reached over a link). Gone counts as settled.
  const std::optional<ThreadMessage> row =
      ports_.get_message ? ports_.get_message(thread_id, message_id) : std::nullopt;
  return !row || row->delivery != MessageDelivery::Pending;
}

void ForwardRun::WakeAfter(const int64_t delay_ms) {
  if (ports_.after && wake_) {
    ports_.after(delay_ms, wake_);
  }
}

bool ForwardRun::InFlight() const {
  return in_flight_;
}

ForwardProgress ForwardRun::Progress() const {
  ForwardProgress progress;
  progress.done = result_.forwarded;
  progress.total = result_.total_items;
  progress.uploading = in_flight_;
  if (completion_) {
    progress.bytes_done = completion_->bytes_done.load();
    progress.bytes_total = completion_->bytes_total.load();
  }
  progress.uploading_since_ms = in_flight_ ? uploading_since_ms_ : 0;
  progress.waiting_for_pace = rate_limited_ && !in_flight_ && !stopped_;
  progress.finishing = stopped_ && in_flight_ && (stop_ == ForwardStop::Cancelled || stop_ == ForwardStop::Tapped);
  progress.can_close =
      progress.finishing && ports_.now_ms && ports_.now_ms() - finishing_since_ms_ >= kForwardCloseOfferMs;
  return progress;
}

void ForwardRun::Stop(const ForwardStop kind) {
  switch (kind) {
    case ForwardStop::Cancelled:
    case ForwardStop::Tapped:
      if (stopped_) {
        return; // already stopping (a harder stop stays)
      }
      stopped_ = true;
      stop_ = kind;
      if (in_flight_) {
        finishing_since_ms_ = ports_.now_ms ? ports_.now_ms() : 0;
        WakeAfter(kForwardCloseOfferMs); // the page then offers "Close"
      }
      break;
    case ForwardStop::Interrupted:
    case ForwardStop::SourceGone:
      stopped_ = true;
      stop_ = kind;
      result_.interrupted = true;
      result_.stop = kind;
      break;
    case ForwardStop::Abandoned:
      // The user left a Cancel nobody answered: the item under way is neither counted nor failed.
      stopped_ = true;
      stop_ = kind;
      result_.stop = kind;
      result_.nothing_sent = result_.forwarded == 0;
      break;
    case ForwardStop::None:
      break;
  }
}

void ForwardRun::Collect() {
  if (!in_flight_ || !completion_ || !completion_->answer) {
    return;
  }
  Roe<ThreadMessage> answer = std::move(*completion_->answer);
  completion_.reset();
  in_flight_ = false;
  if (answer) {
    // Counted also when the run was stopped meanwhile: it did go out.
    ++result_.forwarded;
    uploaded_bytes_ += in_flight_bytes_;
    last_sent_id_ = answer->id;
    last_was_attachment_ = true;
    NextItem();
    return;
  }
  if (IsAttachmentRateLimited(answer.error())) {
    // Refused before anything was stored: the same item goes again, as often as it takes. Not a failure.
    if (!stopped_) {
      rate_limited_ = true;
      retry_at_ms_ = (ports_.now_ms ? ports_.now_ms() : 0) + kForwardRateLimitRetryMs;
      WakeAfter(kForwardRateLimitRetryMs);
    }
    return;
  }
  FailMessage(message_at_);
  last_sent_id_.clear();
  last_was_attachment_ = true;
  NextItem();
}

bool ForwardRun::StartAttachment() {
  const ChatAttachmentFields& fields = *fields_[message_at_];
  const auto fail = [this]() {
    FailMessage(message_at_);
    last_sent_id_.clear();
    last_was_attachment_ = true;
    NextItem();
    return false;
  };
  if (!ports_.local_bytes || ports_.local_bytes(fields.content_hash) == 0 || !ports_.load_plaintext ||
      !ports_.send_attachment) {
    return fail(); // this device does not hold it (never downloaded, or cleaned away): not fetched for a forward
  }
  Roe<ByteVector> plain = ports_.load_plaintext(fields.content_hash);
  if (!plain) {
    return fail(); // stored but cannot be read (incomplete, damaged)
  }
  // The declared size is the sender's word: the real one decides.
  if (plain->size() > kMaxForwardUploadBytes || uploaded_bytes_ + plain->size() > kMaxForwardUploadBytes) {
    return fail();
  }
  OutgoingAttachment attachment;
  attachment.filename = fields.filename;
  attachment.mime = fields.mime;
  attachment.bytes = std::move(*plain);
  attachment.image_is_shrunk = false; // the bytes are final: no shrink, and the 50 MB file limit applies
  in_flight_bytes_ = attachment.bytes.size();
  completion_ = std::make_shared<Completion>();
  completion_->wake = wake_;
  completion_->bytes_total = attachment.bytes.size();
  in_flight_ = true;
  rate_limited_ = false;
  uploading_since_ms_ = ports_.now_ms ? ports_.now_ms() : 0;
  // The callbacks hold only the shared cell: a run that was let go of is not touched by a late answer.
  const std::shared_ptr<Completion> cell = completion_;
  ports_.send_attachment(
      target_thread_, std::move(attachment),
      [cell](Roe<ThreadMessage> sent) {
        cell->answer = std::move(sent);
        if (cell->wake) {
          cell->wake();
        }
      },
      [cell](const uint64_t done, const uint64_t total) {
        cell->bytes_done = done;
        cell->bytes_total = total;
        return true; // an item under way is never interrupted
      });
  return true;
}

ForwardRun::State ForwardRun::Step(size_t budget, const std::function<bool()>& still_valid) {
  if (result_.interrupted) {
    return State::Aborted;
  }
  const auto valid = [&]() {
    if (still_valid && !still_valid()) {
      result_.interrupted = true;
      result_.stop = ForwardStop::Interrupted;
      return false;
    }
    return true;
  };
  if (!valid()) {
    return State::Aborted;
  }
  Collect();
  if (!prepared_ && !Prepare()) {
    result_.nothing_sent = true;
    target_at_ = targets_.size();
    return State::Done;
  }
  while (target_at_ < targets_.size()) {
    if (in_flight_) {
      return State::Waiting;
    }
    if (stopped_) {
      // A Cancel, or a notification tap: nothing under way is left, so it ends here with what went out.
      result_.stop = stop_;
      result_.nothing_sent = result_.forwarded == 0;
      target_at_ = targets_.size();
      return State::Done;
    }
    if (budget == 0) {
      break;
    }
    if (!valid()) {
      return State::Aborted;
    }
    if (rate_limited_ && ports_.now_ms && ports_.now_ms() < retry_at_ms_) {
      return State::Waiting;
    }
    if (!target_open_ && !BeginTarget()) {
      continue;
    }
    if (has_attachments_) {
      // Runs with an attachment keep the chats' order for certain: an attachment goes only after the item before it
      // has left the queue (sent or failed), and the item after it likewise. Each item is also checked again, as it
      // may be minutes since the chat was.
      const bool next_is_attachment = fields_[message_at_].has_value();
      if ((next_is_attachment || last_was_attachment_) && !last_sent_id_.empty() &&
          !Settled(target_thread_, last_sent_id_)) {
        WakeAfter(kForwardSettlePollMs);
        return State::Waiting;
      }
      if (message_at_ > 0 &&
          (!ports_.target_of || ports_.target_of(target_thread_) == ForwardTargetKind::Unavailable)) {
        FailRestOfTarget();
        continue;
      }
      if (next_is_attachment) {
        if (StartAttachment()) {
          return State::Waiting;
        }
        continue;
      }
    }
    const ThreadMessage& message = messages_[message_at_];
    SendRelayOptions options;
    if (message.content_type == ChatContentType::ContactCard) {
      options.content_type = ChatContentType::ContactCard;
      options.payload_json = message.payload_json;
    }
    const std::string& target = target_thread_;
    const auto& send = target_kind_ == ForwardTargetKind::Group ? ports_.send_group : ports_.send_user;
    --budget;
    auto sent =
        send ? send(target, message.text, std::move(options)) : Roe<ThreadMessage>(Error("Messaging not ready"));
    if (sent) {
      ++result_.forwarded;
      last_sent_id_ = sent->id;
    } else {
      last_sent_id_.clear();
      if (!send_failed_[message_at_]) {
        // Counted once per message, however many chats refused it.
        send_failed_[message_at_] = true;
        ++result_.send_errors;
      }
    }
    last_was_attachment_ = false;
    if (++message_at_ >= messages_.size()) {
      target_open_ = false;
      ++target_at_;
      last_sent_id_.clear();
    }
  }
  if (target_at_ < targets_.size()) {
    return State::Running;
  }
  result_.nothing_sent = result_.forwarded == 0;
  return State::Done;
}

bool ForwardSelection::Has(const std::string& message_id) const {
  return std::find(ids_.begin(), ids_.end(), message_id) != ids_.end();
}

void ForwardSelection::Begin(const std::string& thread_id, const std::string& message_id) {
  active_ = true;
  thread_id_ = thread_id;
  ids_.assign(1, message_id);
}

ForwardSelection::Toggle ForwardSelection::Flip(const std::string& message_id) {
  if (const auto it = std::find(ids_.begin(), ids_.end(), message_id); it != ids_.end()) {
    ids_.erase(it);
    return Toggle::Deselected;
  }
  if (ids_.size() >= kMaxForwardMessages) {
    return Toggle::LimitReached;
  }
  ids_.push_back(message_id);
  return Toggle::Selected;
}

void ForwardSelection::Clear() {
  active_ = false;
  thread_id_.clear();
  ids_.clear();
}

} // namespace pbr
