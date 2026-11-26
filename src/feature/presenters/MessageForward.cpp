#include "feature/presenters/MessageForward.h"

#include "lib/delivery/ChatPayloadCodec.h"

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
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return std::tolower(c); });
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
  send_failed_.assign(messages_.size(), false);
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
    } else if (!send_failed_[message_at_]) {
      // Counted once per message, however many chats refused it.
      send_failed_[message_at_] = true;
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
