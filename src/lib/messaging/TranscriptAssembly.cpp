#include "lib/messaging/TranscriptAssembly.h"

#include "lib/messaging/AtAiParser.h"
#include "lib/messaging/ChatPayloadCodec.h"
#include "lib/messaging/GroupMembershipCodec.h"
#include "lib/messaging/QuoteReply.h"
#include "lib/messaging/ReactionTypes.h"

#include "common/EmojiKey.h"
#include "common/chat/MessagingLimits.h"

#include <algorithm>
#include <map>
#include <unordered_map>
#include <unordered_set>

#include "common/PbrCompat.h"

namespace pbr {
namespace {

bool ShownContentType(const ChatContentType type) {
  switch (type) {
  case ChatContentType::Text:
  case ChatContentType::System:
  case ChatContentType::ContactCard:
  case ChatContentType::CryptoTx:
  case ChatContentType::Attachment:
  case ChatContentType::Unsupported:
  case ChatContentType::Deleted:
    return true;
  default:
    return false;
  }
}

ChatAnnotationFields AnnotationFieldsOf(const ThreadMessage& annotation) {
  if (auto fields = ChatPayloadCodec::DecodeAnnotationJson(annotation.payload_json)) {
    return *fields;
  }
  ChatAnnotationFields fallback;
  fallback.annotation_type = kAnnotationTypeReaction;
  fallback.target_message_id = annotation.target_message_id.value_or("");
  fallback.value = annotation.text;
  fallback.text = annotation.text;
  return fallback;
}

/** Each person's latest reaction per (target, emoji); an empty emoji is a cleared one. */
struct LatestReaction {
  std::string emoji;
  std::string sender;
  int64_t display_order = 0;
  bool set = false;
};

} // namespace

std::vector<TranscriptRow> AssembleTranscript(const std::vector<ThreadMessage>& messages,
                                                const SystemControlRegistry* controls, const ThreadKind thread_kind) {
  const bool direct_thread = thread_kind == ThreadKind::Direct;
  // Messages deleted on this device: an id-only tombstone that shows as one line; what hung off
  // them (reactions) is not shown.
  std::unordered_set<std::string> deleted_ids;
  for (const ThreadMessage& message : messages) {
    if (message.content_type == ChatContentType::Deleted) {
      deleted_ids.insert(message.id);
    }
  }

  std::vector<TranscriptRow> entries;
  std::unordered_map<std::string, size_t> entry_by_id;
  std::vector<const ThreadMessage*> annotations;
  std::string pending_ai_question;
  for (const ThreadMessage& message : messages) {
    if (message.target_message_id && deleted_ids.count(*message.target_message_id) != 0) {
      continue;
    }
    if (message.content_type == ChatContentType::Annotation) {
      annotations.push_back(&message);
      continue;
    }
    if (!ShownContentType(message.content_type)) {
      continue;
    }
    const auto family = controls ? controls->Find(message) : std::nullopt;
    if (family && family->on_lane && family->on_lane(message)) {
      continue;  // lane signaling is never a transcript row (an old store may still hold some)
    }
    if (const auto membership = GroupMembershipCodec::ControlTypeFromMessage(message);
        membership && (IsGroupRosterSyncControl(*membership) ||
                       (*membership == GroupMembershipControlType::GroupDissolved && thread_kind != ThreadKind::Group))) {
      continue;  // roster bookkeeping between members (G012 / G013), or the dissolve DM (G016): not something to read
    }
    if (direct_thread && GroupMembershipCodec::IsHiddenInDirectThread(message)) {
      continue;  // a group's event that travelled as a direct message: the group thread has its own line
    }
    TranscriptRow entry;
    entry.message = message;
    entry.control = family.has_value();
    // An "@ai" question and its answer: the answer shows what was asked on top, like a reply.
    if (message.sender_contact_id == kLocalSelfContactId) {
      const AtAiParseResult asked = ParseAtAiPrefix(message.text);
      pending_ai_question.clear();
      if (asked.is_ai_invoke) {
        const auto parts = SplitQuoteReply(asked.prompt);
        pending_ai_question = parts ? parts->reply : asked.prompt;
      }
    } else if (message.sender_contact_id == kAiAssistantContactId && !pending_ai_question.empty()) {
      entry.asked_question = std::move(pending_ai_question);
      pending_ai_question.clear();
    }
    entry_by_id[message.id] = entries.size();
    entries.push_back(std::move(entry));
  }

  std::unordered_map<std::string, size_t> annotations_on_target;
  std::map<std::string, LatestReaction> latest;  // key: target|sender|emoji_key
  std::vector<TranscriptRow> orphans;
  for (const ThreadMessage* annotation : annotations) {
    const ChatAnnotationFields fields = AnnotationFieldsOf(*annotation);
    if (fields.annotation_type == kAnnotationTypeRetract) {
      continue;  // applied to its target (or waiting for it); never shown
    }
    const std::string target_id = annotation->target_message_id.value_or("");
    const auto target = entry_by_id.find(target_id);
    if (target_id.empty() || target == entry_by_id.end()) {
      TranscriptRow orphan;
      orphan.message = *annotation;
      orphan.orphan_annotation = true;
      orphans.push_back(std::move(orphan));
      continue;
    }
    if (annotations_on_target[target_id] >= kMaxAnnotationsPerTarget) {
      continue;
    }
    ++annotations_on_target[target_id];
    if (!IsReactionAnnotationType(fields.annotation_type)) {
      entries[target->second].annotation_badges.push_back(annotation->text);
      continue;
    }
    const std::string emoji_raw = fields.value.empty() ? annotation->text : fields.value;
    const std::string emoji_key = NormalizeEmojiKey(emoji_raw);
    if (emoji_key.empty()) {
      continue;
    }
    LatestReaction& slot = latest[target_id + "|" + annotation->sender_contact_id + "|" + emoji_key];
    if (slot.set && annotation->display_order < slot.display_order) {
      continue;
    }
    slot.set = true;
    slot.display_order = annotation->display_order;
    slot.sender = annotation->sender_contact_id;
    slot.emoji = fields.annotation_type == kAnnotationTypeReactionClear
                     ? std::string()
                     : (annotation->text.empty() ? emoji_raw : annotation->text);
  }

  // Count per (target, emoji key), in the order the keys sort.
  std::map<std::string, std::vector<std::pair<std::string, TranscriptReaction>>> by_target;
  for (const auto& [key, slot] : latest) {
    if (slot.emoji.empty()) {
      continue;  // cleared
    }
    const std::string target_id = key.substr(0, key.find('|'));
    const std::string emoji_key = key.substr(key.rfind('|') + 1);
    auto& chips = by_target[target_id];
    auto chip = std::find_if(chips.begin(), chips.end(), [&](const auto& c) { return c.first == emoji_key; });
    if (chip == chips.end()) {
      chips.push_back({emoji_key, TranscriptReaction{slot.emoji, 0, false}});
      chip = std::prev(chips.end());
    }
    ++chip->second.count;
    chip->second.mine = chip->second.mine || slot.sender == kLocalSelfContactId;
  }
  for (auto& [target_id, chips] : by_target) {
    auto& reactions = entries[entry_by_id.at(target_id)].reactions;
    for (auto& [emoji_key, chip] : chips) {
      reactions.push_back(std::move(chip));
    }
  }

  for (TranscriptRow& orphan : orphans) {
    entries.push_back(std::move(orphan));
  }
  // Orphans were appended last; put every row back where it belongs in time.
  std::stable_sort(entries.begin(), entries.end(), [](const TranscriptRow& a, const TranscriptRow& b) {
    return a.message.display_order < b.message.display_order;
  });
  return entries;
}

} // namespace pbr
