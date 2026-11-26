#include "feature/presenters/MessageForward.h"

#include "lib/delivery/ChatPayloadCodec.h"

#include <algorithm>
#include <utility>
#include "common/PbrCompat.h"

namespace pbr {

bool CanForwardMessage(const ThreadMessage& message) {
  if (IsLocalActionMessage(message) || message.ai_image_json) {
    return false;
  }
  // Ours that is still queued, or did not go out: it has no settled text to hand on (its red mark has its own tap).
  if (message.sender_contact_id == kLocalSelfContactId &&
      (message.delivery == MessageDelivery::Pending || message.delivery == MessageDelivery::Failed)) {
    return false;
  }
  switch (message.content_type) {
    case ChatContentType::Text:
      return !message.text.empty();
    case ChatContentType::ContactCard:
      // Sent again as stored: the card must still read as one (nothing is rebuilt from the contact it names).
      return static_cast<bool>(ChatPayloadCodec::DecodeContactCardJson(message.payload_json));
    case ChatContentType::System:
    case ChatContentType::Annotation:
    case ChatContentType::CryptoTx:
    case ChatContentType::Attachment:
    case ChatContentType::Unsupported:
    case ChatContentType::Deleted:
      break;
  }
  return false;
}

ForwardRun::ForwardRun(ForwardPorts ports, std::string source_thread, std::vector<std::string> message_ids,
                       std::vector<std::string> targets)
    : ports_(std::move(ports)), source_thread_(std::move(source_thread)), message_ids_(std::move(message_ids)) {
  message_count_ = message_ids_.size();
  for (std::string& target : targets) {
    if (std::find(targets_.begin(), targets_.end(), target) == targets_.end()) {
      targets_.push_back(std::move(target));
    }
  }
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
  return !messages_.empty();
}

bool ForwardRun::BeginTarget() {
  target_kind_ = ports_.target_of ? ports_.target_of(targets_[target_at_]) : ForwardTargetKind::Unavailable;
  if (target_kind_ == ForwardTargetKind::Unavailable) {
    ++result_.skipped_targets;
    ++target_at_;
    return false;
  }
  target_open_ = true;
  message_at_ = 0;
  return true;
}

ForwardRun::State ForwardRun::Step(size_t budget, const std::function<bool()>& still_valid) {
  if (result_.interrupted) {
    return State::Aborted;
  }
  const auto valid = [&]() {
    if (still_valid && !still_valid()) {
      result_.interrupted = true;
      return false;
    }
    return true;
  };
  if (!valid()) {
    return State::Aborted;
  }
  if (!prepared_ && !Prepare()) {
    result_.nothing_sent = true;
    target_at_ = targets_.size();
    return State::Done;
  }
  while (budget > 0 && target_at_ < targets_.size()) {
    if (!valid()) {
      return State::Aborted;
    }
    if (!target_open_ && !BeginTarget()) {
      continue;
    }
    const ThreadMessage& message = messages_[message_at_];
    SendRelayOptions options;
    if (message.content_type == ChatContentType::ContactCard) {
      options.content_type = ChatContentType::ContactCard;
      options.payload_json = message.payload_json;
    }
    const std::string& target = targets_[target_at_];
    const auto& send = target_kind_ == ForwardTargetKind::Group ? ports_.send_group : ports_.send_user;
    --budget;
    auto sent =
        send ? send(target, message.text, std::move(options)) : Roe<ThreadMessage>(Error("Messaging not ready"));
    if (sent) {
      ++result_.forwarded;
    } else {
      ++result_.send_errors;
    }
    if (++message_at_ >= messages_.size()) {
      target_open_ = false;
      ++target_at_;
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
