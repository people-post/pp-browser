#include "gui/chat/TranscriptRml.h"
#include "gui/chat/EmojiOnlyScale.h"

#include "gui/chat/ChatActionLabel.h"
#include "gui/chat/DeliveryNoticeText.h"
#include "gui/chat/MessageTimeLabel.h"
#include "feature/presenters/GroupEventText.h"
#include "gui/chat/VoiceBubble.h"
#include "lib/voice/AudioFilePlayer.h"
#include "lib/voice/VoiceFile.h"
#include "common/Utilities.h"
#include "common/ValueJson.h"

#include "lib/ai/StructuredTextParser.h"
#include "lib/calls/CallControlFamily.h"
#include "lib/messaging/AttachmentCache.h"
#include "lib/messaging/EditMessage.h"
#include "lib/delivery/ChatPayloadCodec.h"
#include "lib/messaging/GroupMembershipCodec.h"
#include "lib/messaging/PlainTextLinks.h"
#include "lib/delivery/PskRotateCodec.h"
#include "lib/messaging/QuoteReply.h"
#include "lib/delivery/TextSpans.h"
#include "lib/people/AvatarGlyph.h"
#include "lib/ui/ChatFormHelper.h"
#include "common/chat/MessagingJson.h"
#include "common/ui/WorkingSetCodec.h"
#include "domain/attachments/AttachmentContent.h"
#include "foundation/i18n/LocalizationService.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <memory>
#include <sstream>
#include <unordered_map>

#include "common/PbrCompat.h"

namespace pbr {
namespace {

std::string InlineChatActionButtonsRml(const std::vector<TranscriptChatAction>& chat_actions) {
  std::ostringstream out;
  for (size_t i = 0; i < chat_actions.size(); ++i) {
    out << "<button class=\"chat-suggestion\" data-event-click=\"send_chat_action('__ENTRY__', " << i << ")\">"
        << StructuredTextParser::EscapeText(ChatActionLabel(chat_actions[i])) << "</button>";
  }
  return out.str();
}

std::string HydrateChatActions(const std::string& body_rml, const std::vector<TranscriptChatAction>& chat_actions) {
  if (chat_actions.empty() || body_rml.find("chat-suggestion") != std::string::npos ||
      body_rml.find("chat-working-set-chip") != std::string::npos) {
    return body_rml;
  }
  return body_rml + InlineChatActionButtonsRml(chat_actions);
}

std::string SystemLineRml(const std::string& text) {
  return "<div class=\"chat-system-line muted\"><p>" + StructuredTextParser::EscapeText(text) + "</p></div>";
}

/**
 * A click handler's string argument: backslash-escaped for the invoker's single-quoted literal,
 * then HTML-attribute-escaped — it sits in a double-quoted RML attribute
 * (data-event-click="fn('...')") the DOM parser decodes before the invoker sees it, so a raw '"'
 * would end the attribute whatever the JS-side escaping. Ids and emoji can come from a peer.
 */
std::string JsArgInAttribute(const std::string& value) {
  std::string out;
  out.reserve(value.size());
  for (const char ch : value) {
    switch (ch) {
      case '\\':
        out += "\\\\";
        break;
      case '\'':
        out += "\\'";
        break;
      case '"':
        out += "&quot;";
        break;
      case '<':
        out += "&lt;";
        break;
      case '>':
        out += "&gt;";
        break;
      case '&':
        out += "&amp;";
        break;
      default:
        out.push_back(ch);
        break;
    }
  }
  return out;
}

std::string RowClass(const std::string& sender_contact_id) {
  if (sender_contact_id == kLocalSelfContactId) {
    return "message-row-user";
  }
  if (sender_contact_id == kAiAssistantContactId) {
    return "message-row-ai";
  }
  return "message-row-peer";
}

std::string SharedBadgeRml(const ThreadMessage& message) {
  if (!message.ai_invoke_mode ||
      (message.ai_invoke_mode != "shared_reply" && message.ai_invoke_mode != "shared_full")) {
    return "";
  }
  return "<span class=\"chat-shared-badge muted\">" + Tr("chat.shared_badge") + "</span>";
}

/** A local "couldn't read a message" line: worded from its payload (rows stored before had words). */
std::optional<std::string> ReceiveFailureLine(const ThreadMessage& message) {
  if (!message.text.empty() || message.sender_contact_id != kLocalSelfContactId) {
    return std::nullopt;
  }
  auto payload = TryParseObject(message.payload_json);
  if (!payload || payload->getString("control_type").value_or("") != "receive_failure") {
    return std::nullopt;
  }
  auto detail = TryParseObject(payload->getString("detail").value_or(""));
  if (!detail) {
    return std::nullopt;
  }
  const auto reason = ReceiveFailureFromWire(detail->getString("reason").value_or(""));
  if (!reason) {
    return std::nullopt;
  }
  return ReceiveFailureText(*reason, detail->getString("sender").value_or(""),
                            detail->getString("family").value_or(""));
}

/** A local "missed call (declined by your settings)" line: worded from its payload's count, in the reader's language. */
std::optional<std::string> CallBlockedLine(const ThreadMessage& message) {
  if (message.sender_contact_id != kLocalSelfContactId) {
    return std::nullopt;
  }
  auto payload = TryParseObject(message.payload_json);
  if (!payload || payload->getString("control_type").value_or("") != "call_blocked") {
    return std::nullopt;
  }
  auto detail = TryParseObject(payload->getString("detail").value_or(""));
  const uint64_t count = detail ? detail->getNonNegInt("count").value_or(1) : 1;
  return count <= 1 ? Tr("call.blocked.row.one") : Tr("call.blocked.row.many", {{"n", std::to_string(count)}});
}

/** A local "this message did not go out" line under a message: worded from its payload, in the reader's language. */
std::optional<std::string> SendFailureLine(const ThreadMessage& message, const TranscriptFacts& facts) {
  const auto reason = SendFailureRowReason(message);
  if (!reason) {
    return std::nullopt;
  }
  if (*reason == kGroupMembersMissedWire) {
    return GroupMembersMissedText();
  }
  if (const auto line = facts.send_failure_line ? facts.send_failure_line(message) : std::nullopt) {
    return line;
  }
  return SendFailureText(SendFailureFromWire(*reason).value_or(SendFailure::Unknown));
}

/** A system row's stored text; a line its writer stores in English shows in the UI language. */
std::string StoredSystemText(const ThreadMessage& message) {
  return StoredSystemLineText(message.text).value_or(message.text);
}

/** A call summary: state icon + duration / short outcome word in a small pill on the starter's side (Kenneth 2026-10-23). */
std::string CallRowRml(const CallSummaryRow& row, bool tappable) {
  std::string html = "<div class=\"chat-call-row";
  if (!row.connected) {
    html += " muted";
  }
  if (tappable) {
    // Tap = call back, same type as the record, the way the header's call button starts one.
    html += " chat-call-row--tappable\" data-event-click=\"call_back('";
    html += row.video ? "1" : "0";
    html += "')";
  }
  const CallRowLook look = CallRowLookOf(row);
  html += "\"><svg class=\"chat-call-row-icon\" src=\"../icons/v2/" + look.icon + ".svg\"></svg>";
  html += "<span class=\"chat-call-row-text\">" + StructuredTextParser::EscapeText(look.text) + "</span></div>";
  return html;
}

std::string SystemRml(const TranscriptRow& entry, const TranscriptFacts& facts,
                      const std::optional<CallSummaryRow>& call_row) {
  const ThreadMessage& message = entry.message;
  if (entry.control) {
    if (call_row) {
      return CallRowRml(*call_row, !facts.thread_is_group && !call_row->group);
    }
    // A feature's message: its own line, names included, else the message's text.
    const auto line = facts.control_line ? facts.control_line(message) : std::nullopt;
    return SystemLineRml(line.value_or(StoredSystemText(message)));
  }

  const auto control_type = GroupMembershipCodec::ControlTypeFromMessage(message);
  if (control_type && *control_type == GroupMembershipControlType::GroupInvite) {
    auto invite = GroupMembershipCodec::DecodeInviteFromMessage(message);
    if (const auto resolution = GroupMembershipCodec::InviteResolutionFromMessage(message)) {
      // Answered: the card gives way to a small centred line (Kenneth 2026-11-04: the card spread over the whole
      // width after Accept / Decline), in the user's language; the stored text is the workflow's English.
      const std::string group = invite ? invite->group_title : std::string();
      if (!group.empty() && *resolution == InviteStatus::Accepted) {
        return SystemLineRml(Tr("chat.group_invite.joined", {{"group", group}}));
      }
      if (!group.empty() && *resolution == InviteStatus::Declined) {
        return SystemLineRml(Tr("chat.group_invite.declined", {{"group", group}}));
      }
      return SystemLineRml(StoredSystemText(message));
    }
    const std::string title = invite && !invite->group_title.empty()
                                  ? invite->group_title
                                  : (invite || message.text.empty() ? Tr("chat.group_invite.title") : message.text);
    std::string html = "<div class=\"chat-card chat-group-invite\"><h3 class=\"heading-3\">" +
                       StructuredTextParser::EscapeText(title) + "</h3>";
    // The stored text is English ("Group invitation: <title>") and the title is already the heading.
    // G022: a group made during a call says so (the people in it may not know the maker otherwise).
    const std::string body = !invite                ? message.text
                             : invite->MadeInCall() ? Tr("chat.group_invite.body_call")
                                                    : Tr("chat.group_invite.body");
    html += "<p class=\"text muted\">" + StructuredTextParser::EscapeText(body) + "</p>";
    if (invite && invite->member_count > 0) {
      // G019 3 / 4: how many are in it (never who), and what joining shares.
      const std::string count = std::to_string(invite->member_count);
      html += "<p class=\"text muted\">" +
              StructuredTextParser::EscapeText(Tr("chat.group_invite.member_count", {{"count", count}}) + " " +
                                               Tr("chat.group_invite.shares_profile", {{"count", count}})) +
              "</p>";
    }
    html += HydrateChatActions("", message.chat_actions);
    html += "</div>";
    if (!message.chat_actions.empty() && html.find("__ENTRY__") != std::string::npos) {
      return InjectEntryPlaceholders(html, JsArgInAttribute(message.id));
    }
    return html;
  }
  if (control_type && *control_type == GroupMembershipControlType::GroupOwnerUnreachable) {
    if (GroupMembershipCodec::IsOwnerUnreachableResolved(message)) {
      return SystemLineRml(StoredSystemText(message));
    }
    const std::string body = Tr("chat.group_owner_unreachable.body");
    std::string html = "<div class=\"chat-card chat-group-invite\"><h3 class=\"heading-3\">" +
                       StructuredTextParser::EscapeText(Tr("chat.group_owner_unreachable.title")) + "</h3>";
    html += "<p class=\"text muted\">" + StructuredTextParser::EscapeText(body) + "</p>";
    html += HydrateChatActions("", message.chat_actions);
    html += "</div>";
    if (!message.chat_actions.empty() && html.find("__ENTRY__") != std::string::npos) {
      return InjectEntryPlaceholders(html, JsArgInAttribute(message.id));
    }
    return html;
  }
  if (control_type && *control_type == GroupMembershipControlType::GroupDissolved) {
    // G016: "{owner} dissolved this group" (the owner is the sender), "You dissolved…" on the owner's own device.
    if (message.sender_contact_id == kLocalSelfContactId) {
      return SystemLineRml(Tr("chat.group.dissolved_by_you"));
    }
    std::string name = facts.sender_label ? facts.sender_label(message.sender_contact_id) : std::string();
    return SystemLineRml(name.empty() ? Tr("chat.group.dissolved_line_unnamed")
                                      : Tr("chat.group.dissolved_line", {{"name", name}}));
  }
  if (control_type) {
    if (auto event = GroupEventText(message, facts.member_name, facts.local_account)) {
      return SystemLineRml(*event);
    }
  }
  if (auto failure = ReceiveFailureLine(message)) {
    return SystemLineRml(*failure);
  }
  if (auto blocked = CallBlockedLine(message)) {
    return SystemLineRml(*blocked);
  }
  if (PskRotateCodec::IsPskRotateMessage(message) && message.sender_contact_id != kLocalSelfContactId) {
    return SystemLineRml(Tr("chat.psk_rotate_notice"));
  }
  if (auto failure = SendFailureLine(message, facts)) {
    return SystemLineRml(*failure);
  }
  return SystemLineRml(StoredSystemText(message));
}

std::string ContactCardRml(const ThreadMessage& message) {
  auto fields = ChatPayloadCodec::DecodeContactCardJson(message.payload_json);
  const std::string name =
      fields ? fields->display_name : (message.text.empty() ? Tr("chat.card.contact_fallback") : message.text);
  // The person's own name and Account ID, from the card (checked before *Add*; shown escaped).
  std::string nickname;
  std::string account_id;
  if (fields) {
    if (auto card = TryParseObject(fields->card_json)) {
      nickname = card->getString("nickname").value_or("");
      account_id = card->getString("account_id").value_or("");
    }
  }
  std::string html = "<div class=\"chat-card chat-contact-card\">";
  html += "<h3 class=\"heading-3\">" + StructuredTextParser::EscapeText(name) + "</h3>";
  if (!nickname.empty() && nickname != name) {
    html += "<p class=\"text\">" + StructuredTextParser::EscapeText(nickname) + "</p>";
  }
  if (!account_id.empty()) {
    html += "<p class=\"text muted\">" + StructuredTextParser::EscapeText(account_id) + "</p>";
  }
  html += "</div>";
  return html;
}

std::string CryptoTxRml(const ThreadMessage& message) {
  auto fields = ChatPayloadCodec::DecodeCryptoTxJson(message.payload_json);
  const std::string summary = message.text.empty() ? Tr("chat.card.transaction_fallback") : message.text;
  std::string html = "<div class=\"chat-card chat-tx-card\"><h3 class=\"heading-3\">" +
                     StructuredTextParser::EscapeText(summary) + "</h3>";
  if (fields) {
    html += "<p class=\"text muted\">" + StructuredTextParser::EscapeText(fields->asset) + " · " +
            StructuredTextParser::EscapeText(fields->amount) + " · " +
            StructuredTextParser::EscapeText(fields->direction) + "</p>";
    if (!fields->status.empty()) {
      html += "<p class=\"text muted\">" +
              StructuredTextParser::EscapeText(Tr("chat.card.transaction_status", {{"status", fields->status}})) +
              "</p>";
    }
  }
  html += "</div>";
  return html;
}

/**
 * The photo icon in the middle of a placeholder (static: no animation, the page would redraw every frame). An <svg> and
 * no <div>: the clip's own end is found as the first "</div>" after its marks (PictureClipSpan, DropPictureImages).
 */
constexpr const char* kPicturePlaceholderIconRml =
    "<svg class=\"chat-picture-placeholder-icon\" src=\"../icons/v2/photo.svg\" width=\"28\" height=\"28\"></svg>";

/**
 * A picture fitted into its bubble: a clip box with the fitted size and rounded corners (the engine does not round an
 * <img>), the picture in it (a tall one cropped to its middle), then `overlay`. `src` empty: the box alone (a grey
 * placeholder by its class). `fit` unknown ({0, 0}): the stylesheet fills the box. `img_attrs` go on the <img>.
 */
std::string PictureClipRml(const PictureBubbleSize& fit, const std::string& src, const std::string& clip_class,
                           const std::string& img_attrs, const std::string& overlay = {},
                           const std::string& clip_attrs = {}) {
  std::string html = "<div class=\"chat-attachment-clip" + clip_class;
  if (fit.width_dp > 0) {
    html += "\" style=\"width:" + std::to_string(fit.width_dp) + "dp;height:" + std::to_string(fit.height_dp) + "dp\"";
  } else {
    html += " chat-attachment-clip--auto\""; // unknown size: the stylesheet fills the box
  }
  html += clip_attrs + ">";
  if (!src.empty()) {
    html += "<img class=\"chat-attachment-image\" src=\"" + StructuredTextParser::EscapeText(src) + "\"";
    if (fit.width_dp > 0) {
      html += " width=\"" + std::to_string(fit.width_dp) + "\" height=\"" + std::to_string(fit.image_height_dp) + "\"";
      if (fit.image_offset_dp > 0) {
        html +=
            " style=\"margin-top:-" + std::to_string(fit.image_offset_dp) + "dp\""; // a tall picture shows its middle
      }
    }
    html += img_attrs + "/>";
  }
  return html + overlay + "</div>";
}

} // namespace

std::string EditedTimeLabel(const ThreadMessage& message, const std::string& time_label) {
  if (message.content_type != ChatContentType::Text || !EditMarkOf(message)) {
    return time_label;
  }
  const std::string edited = Tr("chat.message.edited");
  return time_label.empty() ? edited : edited + " " + time_label;
}

std::string PictureBubbleClipRml(const std::string& message_id, const std::string& hash_hex,
                                 const TranscriptAttachmentView& view) {
  // Its size is given, so the frame round it is laid out for that size and never for the file's own; the engine does
  // not round an <img>, so a clip box with the same size and rounded corners holds it. The clip names its message and
  // bytes, so a preview made later replaces only this clip (ReplacePictureClip).
  const PictureBubbleSize fit = FitPictureInBubble(view.image_width, view.image_height);
  const std::string tap = " data-event-click=\"open_picture('" + JsArgInAttribute(message_id) + "')\"";
  const std::string marks = " pic-id=\"" + StructuredTextParser::EscapeText(message_id) + "\" pic-hash=\"" +
                            StructuredTextParser::EscapeText(hash_hex) + "\"";
  if (view.picture_placeholder || view.local_path.empty()) {
    return PictureClipRml(fit, std::string(), " chat-picture-placeholder", std::string(), kPicturePlaceholderIconRml,
                          marks + tap);
  }
  return PictureClipRml(fit, view.local_path, std::string(), tap, std::string(), marks);
}

namespace {

/** Where the picture clip of `message_id` starts and ends ([begin, end)) in a row's RML; nullopt: none. */
std::optional<std::pair<size_t, size_t>> PictureClipSpan(const std::string& rml, const std::string& message_id) {
  const std::string mark = " pic-id=\"" + StructuredTextParser::EscapeText(message_id) + "\"";
  const size_t at = rml.find(mark);
  if (at == std::string::npos) {
    return std::nullopt;
  }
  const size_t begin = rml.rfind("<div class=\"chat-attachment-clip", at);
  // The clip holds an <img .../> or nothing: its own end is the first close after its marks.
  const size_t close = rml.find("</div>", at);
  if (begin == std::string::npos || close == std::string::npos) {
    return std::nullopt;
  }
  return std::make_pair(begin, close + 6);
}

} // namespace

bool ReplacePictureClip(std::string& rml, const std::string& message_id, const std::string& clip_rml) {
  const auto span = PictureClipSpan(rml, message_id);
  if (!span) {
    return false;
  }
  rml.replace(span->first, span->second - span->first, clip_rml);
  return true;
}

bool RowShowsPicturePlaceholder(const MessageDisplayRow& row, const std::string& hash_hex) {
  const std::string_view rml(row.content_rml.data(), row.content_rml.size());
  const std::string mark = "pic-hash=\"" + StructuredTextParser::EscapeText(hash_hex) + "\"";
  for (size_t at = rml.find(mark); at != std::string_view::npos; at = rml.find(mark, at + mark.size())) {
    const size_t begin = rml.rfind("<div class=\"chat-attachment-clip", at);
    const size_t open_end = rml.find('>', at);
    if (begin != std::string_view::npos && open_end != std::string_view::npos &&
        rml.substr(begin, open_end - begin).find("chat-picture-placeholder") != std::string_view::npos) {
      return true;
    }
  }
  return false;
}

ReadyPreviews ApplyReadyPreviews(
    std::vector<MessageDisplayRow>& rows, const std::vector<std::string>& ready,
    const std::unordered_set<std::string>& failed,
    const std::function<std::optional<TranscriptAttachmentView>(const std::string& message_id)>& view_of) {
  ReadyPreviews out;
  for (MessageDisplayRow& row : rows) {
    for (const std::string& hex : ready) {
      if (!RowShowsPicturePlaceholder(row, hex)) {
        continue;
      }
      if (failed.contains(hex)) {
        out.redraw_all = true; // its card becomes a file card
        continue;
      }
      const std::string message_id = row.message_id.c_str();
      const std::optional<TranscriptAttachmentView> view = view_of(message_id);
      if (!view) {
        out.redraw_all = true;
        continue;
      }
      if (view->picture_placeholder) {
        continue; // gone again meanwhile (a lock): asked again, it comes later
      }
      if (view->local_path.empty() || !view->inline_image_ok) {
        out.redraw_all = true;
        continue;
      }
      // Only this picture's clip: the row's element and the other rows stay as they are (data-rml resets this row's
      // content only, as its RML is the only one that changed).
      std::string rml = row.content_rml.c_str();
      if (ReplacePictureClip(rml, message_id, PictureBubbleClipRml(message_id, hex, *view))) {
        row.content_rml = rml.c_str();
        out.changed = true;
      }
    }
  }
  return out;
}

bool DropPictureImages(std::string& rml) {
  bool changed = false;
  static const std::string kClip = "<div class=\"chat-attachment-clip";
  for (size_t begin = rml.find(kClip); begin != std::string::npos; begin = rml.find(kClip, begin + 1)) {
    const size_t open_end = rml.find('>', begin);
    if (open_end == std::string::npos) {
      break;
    }
    const std::string open_tag = rml.substr(begin, open_end - begin);
    if (open_tag.find(" pic-id=\"") == std::string::npos ||
        open_tag.find("chat-picture-placeholder") != std::string::npos) {
      continue; // not a chat picture's clip (the marks are only on those), or already its placeholder
    }
    const size_t close = rml.find("</div>", open_end);
    if (close == std::string::npos) {
      break;
    }
    rml.erase(open_end + 1, close - open_end - 1); // the <img .../> goes: nothing points at the wiped file
    rml.insert(open_end + 1, kPicturePlaceholderIconRml);
    rml.insert(begin + kClip.size(), " chat-picture-placeholder");
    changed = true;
  }
  // A video's poster and a sound file's cover come from the same view cache. The poster's <img> goes and its frame
  // stays empty (VideoBodyRml's frame without a poster); the cover becomes the music note it shows without one.
  // An attribute value never holds "/>" (EscapeText escapes '>'), so the first one after the tag closes it.
  const auto drop_tag = [&rml, &changed](const std::string& tag, const std::string& instead) {
    for (size_t at = rml.find(tag); at != std::string::npos; at = rml.find(tag, at + instead.size())) {
      const size_t end = rml.find("/>", at);
      if (end == std::string::npos) {
        return;
      }
      rml.replace(at, end + 2 - at, instead);
      changed = true;
    }
  };
  drop_tag("<img class=\"chat-attachment-video-poster\"", std::string());
  drop_tag("<img class=\"chat-audio-cover-image\"", kAudioCoverNoteRml);
  return changed;
}

namespace {

/**
 * A video card's body, sent or being sent: the poster in its clip box (`poster_frame`: FitPictureInBubble; 216 x 122
 * when its size is unknown; the box empty when the poster file is gone), or the bare dark frame naming the file; `inner` over it (the play disc, or the
 * upload disc); the size line under it when there is one. `frame_attrs` go on the frame (the tap).
 */
std::string VideoBodyRml(const bool poster_frame, const std::string& poster_path, const int width, const int height,
                         const std::string& frame_attrs, const std::string& inner, const std::string& label,
                         const std::string& size_text, const std::string& poster_attrs = {}) {
  std::string html = "<div class=\"chat-attachment-video\">";
  if (poster_frame) {
    PictureBubbleSize fit = FitPictureInBubble(width, height);
    if (fit.width_dp <= 0) {
      fit = {216, 122, 122, 0};
    }
    html += "<div class=\"chat-attachment-clip chat-attachment-video-frame\" style=\"width:" +
            std::to_string(fit.width_dp) + "dp;height:" + std::to_string(fit.height_dp) + "dp\"" + frame_attrs + ">";
    if (!poster_path.empty()) {
      html += "<img class=\"chat-attachment-video-poster\" src=\"" + StructuredTextParser::EscapeText(poster_path) +
              "\" width=\"" + std::to_string(fit.width_dp) + "\" height=\"" +
              std::to_string(fit.image_height_dp > 0 ? fit.image_height_dp : fit.height_dp) + "\"" +
              (fit.image_offset_dp > 0 ? " style=\"margin-top:-" + std::to_string(fit.image_offset_dp) + "dp\"" : "") +
              poster_attrs + "/>";
    }
    html += inner;
    html += "</div>";
  } else {
    html += "<div class=\"chat-attachment-video-frame chat-attachment-video-frame--bare\"" + frame_attrs + ">";
    html += "<div class=\"chat-attachment-video-badge\">" +
            StructuredTextParser::EscapeText(Tr("chat.card.video_badge")) + "</div>";
    html += "<p class=\"text\">" + StructuredTextParser::EscapeText(label) + "</p>";
    html += inner;
    html += "</div>";
  }
  if (!size_text.empty()) {
    html += "<p class=\"text muted\">" + StructuredTextParser::EscapeText(size_text) + "</p>";
  }
  return html + "</div>";
}

/** The file type tile's face: the name's extension in capitals (at most 4 letters), else the generic file icon. */
std::string FileTileFaceRml(const std::string& name) {
  const std::string label = FileTypeTileLabel(name);
  if (label.empty()) {
    return "<svg class=\"chat-file-tile-icon\" src=\"../icons/v2/file.svg\" width=\"30\" height=\"30\"></svg>";
  }
  return "<span class=\"chat-file-tile-label\">" + StructuredTextParser::EscapeText(label) + "</span>";
}

/**
 * A file card, sent or being sent (one generator, so a bubble and the sent card it turns into have the same size): a
 * 72dp type tile on the left (`tile_inner` drawn over it: the upload disc), the name (two lines at most) and the size
 * line on the right, `after` last. `row_attrs` go on the row that fills the card (the sent card's tap opens the file),
 * not on the card: reaction chips hung on the card are its siblings, so a tap on one never opens the file.
 */
std::string FileCardRml(const std::string& name, const std::string& size_text, const std::string& row_attrs,
                        const std::string& tile_inner, const std::string& after) {
  return "<div class=\"chat-card chat-attachment-card chat-file-card\"><div class=\"chat-file-row\"" + row_attrs +
         "><div class=\"chat-file-tile\">" + FileTileFaceRml(name) + tile_inner +
         "</div><div class=\"chat-file-text\"><p class=\"chat-file-name\">" + StructuredTextParser::EscapeText(name) +
         "</p><p class=\"chat-file-size\">" + StructuredTextParser::EscapeText(size_text) + "</p></div></div>" + after +
         "</div>";
}

/** The upload disc of item `upload`: the ring (the percent) and `core` in its middle; uploading: cancel, failed: retry. */
std::string PendingDiscRml(const PendingUpload& upload, const std::string& core) {
  const bool failed = upload.state == PendingUploadState::Failed;
  const bool cancelling = upload.state == PendingUploadState::Cancelling;
  std::string disc = "<button class=\"chat-pending-disc";
  if (cancelling) {
    disc += " chat-pending-disc--busy\" type=\"button\">";
  } else if (failed) {
    disc += "\" type=\"button\" title=\"" + StructuredTextParser::EscapeText(Tr("chat.attachment.retry")) +
            "\" data-event-click=\"retry_attachment_send('" + JsArgInAttribute(upload.row_id) + "')\">";
  } else {
    disc += "\" type=\"button\" title=\"" + StructuredTextParser::EscapeText(Tr("chat.attachment.cancel_send")) +
            "\" data-event-click=\"cancel_attachment_send('" + JsArgInAttribute(upload.row_id) + "')\">";
  }
  if (!failed) {
    disc += "<div id=\"" + PendingRingElementId(upload.row_id) + "\" class=\"chat-pending-ring";
    if (kPendingProgressRing && !cancelling) {
      disc += "\" style=\"decorator: " + PendingRingDecorator(upload.percent) + "\"></div>";
    } else {
      disc += " chat-pending-ring--spin\"></div>";
    }
  }
  return disc + "<div class=\"chat-pending-core\">" + core + "</div></button>";
}

/** The corner ✕ that gives a failed item up (a 44dp target). */
std::string PendingDiscardRml(const PendingUpload& upload, const char* extra_class) {
  return std::string("<button class=\"chat-pending-discard") + extra_class + "\" type=\"button\" title=\"" +
         StructuredTextParser::EscapeText(Tr("common.cancel")) + "\" data-event-click=\"discard_attachment_send('" +
         JsArgInAttribute(upload.row_id) +
         "')\"><div class=\"chat-pending-discard-disc\">"
         "<svg class=\"chat-pending-icon\" src=\"../icons/v2/close.svg\" width=\"20\" height=\"20\"></svg>"
         "</div></button>";
}

std::string AttachmentRml(const ThreadMessage& message, const TranscriptFacts& facts) {
  auto fields = ChatPayloadCodec::DecodeAttachmentJson(message.payload_json);
  const std::string label = fields && !fields->filename.empty()
                                ? fields->filename
                                : (message.text.empty() ? Tr("chat.card.attachment_fallback") : message.text);

  const TranscriptAttachmentView view =
      fields && facts.attachment ? facts.attachment(message) : TranscriptAttachmentView{};
  const AttachmentFetchWorkflow::DownloadState state = view.state;
  const std::string& local_path = view.local_path;

  const bool ready = fields && state == AttachmentFetchWorkflow::DownloadState::Ready;
  const bool inline_image = ready && IsAttachmentImageMime(fields->mime) &&
                            (!local_path.empty() || view.picture_placeholder) && view.inline_image_ok;
  // A picture card is a thin frame around the picture on phones (its own modifier); files and videos are compact cards.
  // A video card is a frame round its poster like a picture's (--video: up to the picture's width, not the file 240dp).
  const bool video_card = ready && !inline_image && IsAttachmentVideoMime(fields->mime);
  std::string html = inline_image ? "<div class=\"chat-card chat-attachment-card chat-attachment-card--image\">"
                     : video_card ? "<div class=\"chat-card chat-attachment-card chat-attachment-card--video\">"
                                  : "<div class=\"chat-card chat-attachment-card\">";
  // A voice message (audio/ogg named voice-<ms>.ogg, at most 1 MB declared) is a bubble; any other audio is a file row.
  // Only the declared values decide here: the file itself is not looked at until the person taps play.
  const VoiceBubbleView voice =
      ready && facts.voice_supported
          ? DecideVoiceBubble(fields->mime, fields->filename, fields->byte_length, facts.voice_max_seconds)
          : VoiceBubbleView{};
  if (voice.bubble) {
    VoiceBubbleFace face;
    face.ours = message.sender_contact_id == kLocalSelfContactId;
    const std::string name = facts.sender_label ? facts.sender_label(message.sender_contact_id) : std::string();
    const AvatarGlyph glyph = MakeAvatarGlyph(
        name, facts.sender_stable_id ? facts.sender_stable_id(message.sender_contact_id) : std::string());
    face.letter = glyph.letter;
    face.tone = glyph.tone;
    if (facts.sender_icon) {
      face.icon_path = facts.sender_icon(message.sender_contact_id);
    }
    face.clock = MessageClockLabel(message.timestamp);
    return VoiceBubbleRml(message.id, JsArgInAttribute(message.id), voice.duration_ms,
                          facts.voice_state ? facts.voice_state(message.id) : VoiceBubbleState{}, face);
  }
  if (ready && facts.audio_files_play && IsAudioFileAttachment(fields->mime, fields->filename) &&
      (!local_path.empty() || view.on_demand)) {
    // A music card: the file's own title and artist when its tags name them (read on this device), else its name.
    AudioCardFace face;
    face.title = view.audio_title.empty() ? label : view.audio_title;
    const std::string size = fields->byte_length > 0 ? FormatAttachmentByteSize(fields->byte_length) : std::string();
    face.subtitle =
        view.audio_artist.empty() ? size : (size.empty() ? view.audio_artist : view.audio_artist + " · " + size);
    face.cover_path = view.poster_path;
    face.duration_ms = view.audio_duration_ms;
    return AudioFileBubbleRml(message.id, JsArgInAttribute(message.id), face,
                              facts.voice_state ? facts.voice_state(message.id) : VoiceBubbleState{});
  }
  if (inline_image) {
    // A tap opens the full picture in the system viewer (zoomable). The bubble shows its preview (or the picture, when
    // no longer than the preview) fitted inside the box, or a placeholder of that size until the preview is made.
    html += PictureBubbleClipRml(message.id, AttachmentHashHex(fields->content_hash), view);
  } else if (ready && IsAttachmentVideoMime(fields->mime)) {
    // The whole frame is the tap target (the system player opens directly); the play disc is drawn over the poster
    // and takes no taps. The frame is clipped to the same size picture bubbles use, so nothing can exceed the bubble.
    const char* play_disc =
        "<div class=\"chat-attachment-video-play\"><svg class=\"chat-attachment-video-play-icon\" "
        "src=\"../icons/play.svg\" width=\"22\" height=\"22\"></svg></div>";
    html +=
        VideoBodyRml(!view.poster_path.empty(), view.poster_path, view.image_width, view.image_height,
                     " data-event-click=\"open_attachment('" + JsArgInAttribute(message.id) + "')\"", play_disc, label,
                     fields->byte_length > 0 ? FormatAttachmentByteSize(fields->byte_length) : std::string());
  } else if (ready && (!local_path.empty() || view.on_demand)) {
    // The whole card opens the file (DEV/PLAN-attachment-bubbles-multi-pick.md): its type tile, name and size.
    return FileCardRml(label, fields->byte_length > 0 ? FormatAttachmentByteSize(fields->byte_length) : std::string(),
                       " data-event-click=\"open_attachment('" + JsArgInAttribute(message.id) + "')\"", {}, {});
  } else {
    html += "<h3 class=\"heading-3\">" + StructuredTextParser::EscapeText(label) + "</h3>";
    if (fields && fields->byte_length > 0) {
      html += "<p class=\"text muted\">" +
              StructuredTextParser::EscapeText(FormatAttachmentByteSize(fields->byte_length)) + "</p>";
    }
    if (state == AttachmentFetchWorkflow::DownloadState::TooLarge) {
      html += "<p class=\"text muted\">" + Tr("chat.attachment.too_large_receive") + "</p>";
    } else if (state == AttachmentFetchWorkflow::DownloadState::Downloading) {
      html += "<p class=\"text muted\">" +
              (view.download_percent >= 0
                   ? Tr("chat.attachment.downloading_percent", {{"percent", std::to_string(view.download_percent)}})
                   : Tr("chat.attachment.downloading")) +
              "</p>";
    } else if (state == AttachmentFetchWorkflow::DownloadState::Pending) {
      html += "<button class=\"chat-suggestion\" type=\"button\" data-event-click=\"download_attachment('" +
              JsArgInAttribute(message.id) + "')\">" + Tr("chat.attachment.download") + "</button>";
    } else {
      html += "<p class=\"text muted\">" + Tr("chat.attachment.download_failed") + "</p>";
      html += "<button class=\"chat-suggestion\" type=\"button\" data-event-click=\"retry_attachment('" +
              JsArgInAttribute(message.id) + "')\">" + Tr("chat.attachment.retry") + "</button>";
    }
  }
  html += "</div>";
  return html;
}

std::string UnsupportedRml() {
  return "<div class=\"chat-card chat-unsupported-card\"><p class=\"text muted\">" +
         StructuredTextParser::EscapeText(Tr("chat.card.unsupported")) + "</p></div>";
}

/** One tappable or highlighted piece of a message's text: a link, or a mention the roster allows. */
struct TextMark {
  size_t begin = 0;
  size_t end = 0;
  std::string rml_open;
  /** What it shows: a link's display form; a mention, exactly the bytes it covers. */
  std::string shown;
};

// Escaped text with its https links tappable and its eligible mentions highlighted. A link's click carries
// the message id and the link's index; the handler finds the URL again in the stored text (PlainTextLinks.h),
// so none sits in the markup. A mention's click carries its index among `mentions` (EligibleMentionRuns), so
// the handler finds the account again the same way. `text` must be the message text or a prefix of it, so the
// indexes agree. Ranges are UTF-8 bytes; a mention that crosses a link is not highlighted (the link wins).
std::string LinkedTextRml(const std::string& text, const std::string& id_arg,
                          const std::vector<MentionRun>& mentions = {}) {
  const std::vector<PlainTextLink> links = FindPlainTextLinks(text);
  if ((links.empty() && mentions.empty()) || id_arg.empty()) {
    return StructuredTextParser::EscapeText(text);
  }
  std::vector<TextMark> marks;
  for (size_t i = 0; i < links.size(); ++i) {
    marks.push_back({links[i].begin, links[i].end,
                     "<span class=\"chat-link\" data-event-click=\"open_message_link('" + id_arg + "', " +
                         std::to_string(i) + ")\">",
                     links[i].display});
  }
  for (size_t i = 0; i < mentions.size(); ++i) {
    const MentionRun& run = mentions[i];
    const bool crosses_link = std::any_of(links.begin(), links.end(), [&run](const PlainTextLink& link) {
      return run.begin < link.end && link.begin < run.end;
    });
    if (crosses_link || run.end > text.size()) {
      continue;
    }
    marks.push_back({run.begin, run.end,
                     run.kind == TextSpanKind::Mention
                         ? "<span class=\"chat-mention\" data-event-click=\"open_mention('" + id_arg + "', " +
                               std::to_string(i) + ")\">"
                         : std::string("<span class=\"chat-mention chat-mention--everyone\">"),
                     text.substr(run.begin, run.end - run.begin)});
  }
  std::sort(marks.begin(), marks.end(), [](const TextMark& a, const TextMark& b) { return a.begin < b.begin; });
  std::string out;
  size_t pos = 0;
  for (const TextMark& mark : marks) {
    out += StructuredTextParser::EscapeText(text.substr(pos, mark.begin - pos));
    out += mark.rml_open + StructuredTextParser::EscapeText(mark.shown) + "</span>";
    pos = mark.end;
  }
  out += StructuredTextParser::EscapeText(text.substr(pos));
  return out;
}

// A quote shows at most two lines: line breaks would waste them.
std::string OneLine(std::string text) {
  std::replace(text.begin(), text.end(), '\n', ' ');
  std::replace(text.begin(), text.end(), '\r', ' ');
  return text;
}

/** A group text's mentions the roster allows now (G015); none in a direct chat, where they are plain text. */
std::vector<MentionRun> MentionRunsOf(const ThreadMessage& message, const std::optional<MentionRoster>& roster) {
  if (!roster || message.content_type != ChatContentType::Text || message.payload_json.empty()) {
    return {};
  }
  const std::string sender =
      message.sender_contact_id == kLocalSelfContactId ? roster->local_account : message.sender_contact_id;
  return EligibleMentionRuns(message.text, MentionTextLimit(message.text),
                             text_spans::FromPayloadJson(message.payload_json), *roster, sender);
}

/** A peer's text that mentions this account, `@everyone` included (only the owner's is eligible): its row is tinted. */
bool MentionsLocalAccount(const ThreadMessage& message, const std::vector<MentionRun>& runs,
                          const std::optional<MentionRoster>& roster) {
  if (!roster || message.sender_contact_id == kLocalSelfContactId) {
    return false;
  }
  return std::any_of(runs.begin(), runs.end(), [&roster](const MentionRun& run) {
    return run.kind == TextSpanKind::Everyone || run.account_id == roster->local_account;
  });
}

// `mentions_me`: set when the text mentions this account (the row is then tinted).
std::string MessageRml(const TranscriptRow& entry, const TranscriptFacts& facts,
                       const std::optional<CallSummaryRow>& call_row, const std::optional<MentionRoster>& mentions,
                       bool& mentions_me) {
  const ThreadMessage& message = entry.message;
  if (message.content_rml) {
    std::string rml = *message.content_rml;
    if (rml.find("__ENTRY__") != std::string::npos) {
      rml = InjectEntryPlaceholders(rml, JsArgInAttribute(message.id));
    }
    const bool has_snapshot = message.working_set_json && !message.working_set_json->empty() &&
                              !WorkingSetCandidatesFromJson(*message.working_set_json).empty();
    if (ContentRmlHasActiveWorkingSetChip(rml) && !has_snapshot) {
      rml = MarkWorkingSetChipsUnavailable(rml);
    }
    return rml;
  }
  if (message.content_type == ChatContentType::System) {
    return SystemRml(entry, facts, call_row);
  }
  if (message.content_type == ChatContentType::Deleted) {
    // On the sender's side, small and grey, so it is clear whose message it was.
    const std::string side = message.sender_contact_id == kLocalSelfContactId ? "bubble-user" : "bubble-assistant";
    return "<div class=\"bubble " + side + " bubble-deleted\"><p class=\"bubble-deleted-text\">" +
           StructuredTextParser::EscapeText(Tr("chat.message.deleted")) + "</p></div>";
  }
  if (message.content_type == ChatContentType::ContactCard) {
    return ContactCardRml(message);
  }
  if (message.content_type == ChatContentType::CryptoTx) {
    return CryptoTxRml(message);
  }
  if (message.content_type == ChatContentType::Attachment) {
    return AttachmentRml(message, facts);
  }
  if (message.content_type == ChatContentType::Unsupported) {
    return UnsupportedRml();
  }
  const std::string bubble_class =
      message.sender_contact_id == kLocalSelfContactId ? "bubble-user" : "bubble-assistant";
  const std::string paragraph =
      message.sender_contact_id == kLocalSelfContactId ? "<p class=\"bubble-text\">" : "<p class=\"bubble-text-peer\">";
  std::string badges = SharedBadgeRml(message);
  if (!badges.empty()) {
    badges = "<div class=\"chat-message-meta\">" + badges + "</div>";
  }
  // Message ids are generated locally or validated on receive; still keep the click argument to id characters.
  std::string id_arg;
  for (const char ch : message.id) {
    if (std::isalnum(static_cast<unsigned char>(ch)) || ch == '-' || ch == '_') {
      id_arg.push_back(ch);
    }
  }
  // A reply that quotes a message: the quote as a block on top (no "> " markers shown), the reply under it.
  // Mentions mark the reply's own words only (MentionTextLimit), so their byte offsets hold in either branch.
  const std::vector<MentionRun> mention_runs = MentionRunsOf(message, mentions);
  mentions_me = MentionsLocalAccount(message, mention_runs, mentions);
  std::string text_rml;
  if (const auto parts = SplitQuoteReply(message.text)) {
    text_rml = "<div class=\"chat-quote chat-quote--jump\" data-event-click=\"jump_to_quote('" + id_arg +
               "')\"><p class=\"chat-quote-text\">" + StructuredTextParser::EscapeText(OneLine(parts->quote)) +
               "</p></div>" + paragraph + LinkedTextRml(parts->reply, id_arg, mention_runs) + "</p>";
  } else {
    text_rml = paragraph + LinkedTextRml(message.text, id_arg, mention_runs) + "</p>";
  }
  // The time sits at the bottom right inside the bubble (phones; hidden on the desktop, which has the meta line). An
  // edited message says so before it, in the same size and colour (DEV/PLAN-message-edit.md).
  const std::string clock = EditedTimeLabel(message, MessageClockLabel(message.timestamp));
  if (!clock.empty()) {
    text_rml += "<div class=\"bubble-time\">" + clock + "</div>";
  }
  // Only emoji (no quote, 1-6 of them): drawn large without a bubble (EmojiOnlyScale).
  std::string emoji_class;
  if (!SplitQuoteReply(message.text)) {
    const EmojiScale scale = EmojiOnlyScale(message.text);
    if (scale != EmojiScale::None) {
      emoji_class = scale == EmojiScale::ExtraLarge ? " bubble-emoji bubble-emoji--xl" : " bubble-emoji";
    }
  }
  std::string body =
      badges + "<div class=\"bubble " + bubble_class + emoji_class + "\" selectable=\"text\">" + text_rml + "</div>";
  body = HydrateChatActions(body, message.chat_actions);
  // __ENTRY__ is only ever introduced by HydrateChatActions' own template above; never treat a
  // literal "__ENTRY__" typed into message.text (peer-controlled) as a placeholder to fill in.
  if (!message.chat_actions.empty() && body.find("__ENTRY__") != std::string::npos) {
    return InjectEntryPlaceholders(body, JsArgInAttribute(message.id));
  }
  return body;
}

// Reaction chips. `overlay`: the row sits inside the bubble / card, drawn as small pills over its bottom edge
// (position: absolute, so the row's height is the bubble's and avatar, tail and bubble bottom line up). Otherwise
// (no bubble or card to hang it on) it follows the content as a line of its own, which marks where the body ends.
// Each chip is a 32dp-tall button whose pill is drawn smaller inside it.
std::string ReactionsRml(const std::string& target_id, const std::vector<TranscriptReaction>& reactions,
                         const bool overlay) {
  std::ostringstream rml;
  // menu-anchor-end: the message menu opens right under the bubble, over this row (ContextMenuHost).
  rml << (overlay ? "<div class=\"chat-reaction-row chat-reaction-row--overlay\">"
                  : "<div class=\"chat-reaction-row\" menu-anchor-end=\"\">");
  for (const TranscriptReaction& chip : reactions) {
    rml << "<button class=\"chat-reaction-chip";
    if (chip.mine) {
      rml << " chat-reaction-chip--mine";
    }
    rml << "\" type=\"button\" data-event-click=\"open_reactors('" << JsArgInAttribute(target_id)
        << "')\"><span class=\"chat-reaction-pill\">" << StructuredTextParser::EscapeText(chip.emoji);
    if (chip.count > 1) {
      rml << "<span class=\"chat-reaction-count\">" << chip.count << "</span>";
    }
    rml << "</span></button>";
  }
  rml << "</div>";
  return rml.str();
}

// Puts the reaction row inside the first bubble or card of `content` (marked chat-reaction-host, the row's
// positioning box); false when there is none, and `content` is unchanged.
bool AddReactionsToHost(std::string& content, const std::string& target_id,
                        const std::vector<TranscriptReaction>& reactions) {
  size_t at = std::string::npos;
  for (const char* opener : {"<div class=\"bubble ", "<div class=\"chat-card"}) {
    const size_t found = content.find(opener);
    if (found != std::string::npos && found < at) {
      at = found;
    }
  }
  const size_t tag_end = at == std::string::npos ? at : content.find('>', at);
  if (tag_end == std::string::npos) {
    return false;
  }
  content.insert(tag_end + 1, ReactionsRml(target_id, reactions, true));
  content.insert(content.find('"', at + std::string("<div class=\"").size()), " chat-reaction-host");
  return true;
}

MessageDisplayRow OrphanAnnotationRow(const TranscriptRow& entry, const TranscriptFacts& facts) {
  const ThreadMessage& annotation = entry.message;
  MessageDisplayRow row;
  row.message_id = annotation.id.c_str();
  row.display_order = annotation.display_order;
  row.sender_label = facts.sender_label ? facts.sender_label(annotation.sender_contact_id).c_str() : "";
  const std::string text = annotation.text.empty() ? "?" : annotation.text;
  row.content_rml = ("<div class=\"chat-orphan-annotation muted\"><span class=\"chat-annotation-badge\">" +
                     StructuredTextParser::EscapeText(text) + "</span> <span class=\"text-xs\">" +
                     StructuredTextParser::EscapeText(Tr("chat.annotation.unlinked")) + "</span></div>")
                        .c_str();
  row.row_class = "message-row-annotation-orphan";
  row.has_content = true;
  return row;
}

// The last bubble of a run gets a speech-bubble tail: a small element at the bubble's bottom corner (received: left,
// sent: right) plus classes that square that corner. Bubbles without a background (deleted) get none.
bool AddBubbleTail(std::string& content, bool ours) {
  size_t at = std::string::npos;
  for (const char* opener : {"<div class=\"bubble ", "<div class=\"chat-card chat-attachment-card",
                             "<div class=\"chat-card chat-voice-card"}) {
    const size_t found = content.find(opener);
    if (found != std::string::npos && found < at) {
      at = found;
    }
  }
  const size_t tag_end = at == std::string::npos ? at : content.find('>', at);
  if (tag_end == std::string::npos || content.substr(at, tag_end - at).find("bubble-deleted") != std::string::npos) {
    return false;
  }
  const char* host = ours ? " bubble-tail-host bubble-tail-host--out" : " bubble-tail-host bubble-tail-host--in";
  const char* tail = ours ? "<svg class=\"bubble-tail bubble-tail-out\" src=\"../icons/v2/bubble-tail-right.svg\" "
                            "width=\"16\" height=\"20\"></svg>"
                          : "<svg class=\"bubble-tail bubble-tail-in\" src=\"../icons/v2/bubble-tail-left.svg\" "
                            "width=\"16\" height=\"20\"></svg>";
  content.insert(tag_end + 1, tail);
  content.insert(content.find('"', at + std::string("<div class=\"").size()), host);
  return true;
}

// #210: our message's send mark, just left of its bubble or card (inline before it, so the row's right alignment keeps
// it beside the bubble's bottom). A clock while the send is under way (not tappable); the red mark when it did not go
// out, a 44dp button whose tap says why (show_send_failure, ChatController). Nothing for SendMark::None.
void AddSendMark(std::string& content, const SendMark mark, const std::string& message_id) {
  if (mark == SendMark::None) {
    return;
  }
  std::string rml;
  if (mark == SendMark::Queued) {
    rml = "<div class=\"send-mark send-mark--queued\" title=\"" +
          StructuredTextParser::EscapeText(Tr("chat.send.mark.queued")) +
          "\"><svg class=\"send-mark-icon\" src=\"../icons/v2/clock.svg\" width=\"16\" height=\"16\"></svg></div>";
  } else {
    rml = "<button class=\"send-mark send-mark--failed\" type=\"button\" title=\"" +
          StructuredTextParser::EscapeText(Tr("chat.send.mark.failed")) + "\" data-event-click=\"show_send_failure('" +
          JsArgInAttribute(message_id) +
          "')\"><svg class=\"send-mark-icon\" src=\"../icons/v2/alert-circle.svg\" width=\"20\" "
          "height=\"20\"></svg></button>";
  }
  size_t at = std::string::npos;
  for (const char* opener : {"<div class=\"bubble ", "<div class=\"chat-card"}) {
    const size_t found = content.find(opener);
    if (found != std::string::npos && found < at) {
      at = found;
    }
  }
  content.insert(at == std::string::npos ? 0 : at, rml);
}

} // namespace

PictureBubbleSize FitPictureInBubble(const int width_px, const int height_px) {
  if (width_px <= 0 || height_px <= 0) {
    return {};
  }
  const double width_scale = static_cast<double>(kPictureBubbleMaxWidthDp) / width_px;
  double scale = std::min(1.0, width_scale);
  const int short_side = std::min(width_px, height_px);
  if (short_side * scale < kPictureBubbleMinShortSideDp) {
    // A small picture grows to a sane short side, but never past the box width.
    scale = std::min(width_scale, static_cast<double>(kPictureBubbleMinShortSideDp) / short_side);
  }
  PictureBubbleSize out;
  out.width_dp = std::max(1, static_cast<int>(std::lround(width_px * scale)));
  out.image_height_dp = std::max(1, static_cast<int>(std::lround(height_px * scale)));
  out.height_dp = std::min(out.image_height_dp, kPictureBubbleMaxHeightDp);
  out.image_offset_dp = (out.image_height_dp - out.height_dp) / 2;
  return out;
}

std::string PendingRingDecorator(int percent) {
  percent = std::clamp(percent, 0, 100);
  if (percent == 0) {
    return "none";
  }
  if (percent == 100) {
    return "conic-gradient(white 0%, white 100%)";
  }
  const std::string at = std::to_string(percent) + "%";
  return "conic-gradient(white 0%, white " + at + ", transparent " + at + ")";
}

std::string PendingRingElementId(const std::string& row_id) {
  return row_id + "-ring";
}

std::string PendingPercentElementId(const std::string& row_id) {
  return row_id + "-percent";
}

std::string PendingThumbElementId(const std::string& row_id) {
  return row_id + "-thumb";
}

std::string PendingPercentText(const int percent) {
  return std::to_string(std::clamp(percent, 0, 100)) + "%";
}

std::string PendingPictureRml(const PendingUpload& upload) {
  const bool failed = upload.state == PendingUploadState::Failed;
  const bool cancelling = upload.state == PendingUploadState::Cancelling;
  // The disc: the whole 48dp is the target (uploading: cancel; failed: retry; cancelling: nothing to tap).
  std::string core;
  if (failed) {
    core = "<svg class=\"chat-pending-icon\" src=\"../icons/v2/retry.svg\" width=\"20\" height=\"20\"></svg>";
  } else if (!cancelling) {
    core = "<svg class=\"chat-pending-icon\" src=\"../icons/v2/close.svg\" width=\"18\" height=\"18\"></svg>";
  }
  std::string overlay = "<div class=\"chat-pending-center\">" + PendingDiscRml(upload, core);
  if (!failed) {
    overlay += "<p id=\"" + PendingPercentElementId(upload.row_id) + "\" class=\"chat-pending-percent\">" +
               PendingPercentText(upload.percent) + "</p>";
  }
  overlay += "</div>";
  if (failed) {
    overlay += PendingDiscardRml(upload, "");
  }
  if (upload.kind == PendingUploadKind::Video) {
    // The sent video's card and frame (VideoBodyRml): the poster, or the bare frame when there is none; no play key
    // while it uploads. Built once the poster is known, so its size never changes.
    return "<div class=\"chat-card chat-attachment-card chat-attachment-card--video chat-pending-video\">" +
           VideoBodyRml(upload.size_known, upload.thumb_path, upload.pixels.width, upload.pixels.height, "", overlay,
                        upload.title, upload.size_text.empty() ? std::string("…") : upload.size_text,
                        " id=\"" + PendingThumbElementId(upload.row_id) + "\"") +
           "</div>";
  }
  const PictureBubbleSize fit = FitPictureInBubble(upload.pixels.width, upload.pixels.height);
  return "<div class=\"chat-card chat-attachment-card chat-attachment-card--image chat-pending-picture\">" +
         PictureClipRml(fit, upload.thumb_path, " chat-pending-picture-clip",
                        " id=\"" + PendingThumbElementId(upload.row_id) + "\"", overlay) +
         "</div>";
}

std::string PendingFileRml(const PendingUpload& upload) {
  const bool failed = upload.state == PendingUploadState::Failed;
  // The percent sits in the disc's middle (no cross: the disc itself is the cancel target).
  const std::string core =
      failed ? "<svg class=\"chat-pending-icon\" src=\"../icons/v2/retry.svg\" width=\"20\" height=\"20\"></svg>"
             : "<span id=\"" + PendingPercentElementId(upload.row_id) + "\" class=\"chat-file-percent\">" +
                   PendingPercentText(upload.percent) + "</span>";
  std::string tile_inner = "<div class=\"chat-pending-center\">" + PendingDiscRml(upload, core) + "</div>";
  if (failed) {
    tile_inner += PendingDiscardRml(upload, " chat-file-discard");
  }
  return FileCardRml(upload.title, upload.size_text.empty() ? std::string("…") : upload.size_text, "", tile_inner, {});
}

std::string PendingUploadRml(const PendingUpload& upload) {
  return upload.kind == PendingUploadKind::File ? PendingFileRml(upload) : PendingPictureRml(upload);
}

std::string FileTypeTileLabel(const std::string& name) {
  const size_t dot = name.rfind('.');
  if (dot == std::string::npos || dot == 0 || dot + 1 >= name.size()) {
    return {};
  }
  std::string ext = name.substr(dot + 1);
  if (ext.size() > 4) {
    return {};
  }
  for (char& ch : ext) {
    if (!std::isalnum(static_cast<unsigned char>(ch))) {
      return {};
    }
    ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
  }
  return ext;
}

std::vector<MessageDisplayRow> RenderTranscript(const std::vector<TranscriptRow>& entries,
                                                const TranscriptFacts& facts_in,
                                                const std::optional<MentionRoster>& mentions) {
  // A name is looked up (cards, contacts: SQLite) once per account in this build, however many lines name it.
  TranscriptFacts facts = facts_in;
  if (facts_in.member_name) {
    auto names = std::make_shared<std::unordered_map<std::string, std::string>>();
    facts.member_name = [lookup = facts_in.member_name, names](const std::string& account_id) {
      auto it = names->find(account_id);
      if (it == names->end()) {
        it = names->emplace(account_id, lookup(account_id)).first;
      }
      return it->second;
    };
  }
  std::vector<MessageDisplayRow> rows;
  rows.reserve(entries.size());
  // Phone layout: date strips and runs. A run is consecutive plain messages from one sender, each within
  // kRunGapMs of the one before and on the same day; a system / call row or a day change ends it.
  constexpr int64_t kRunGapMs = 5 * 60 * 1000;
  const bool group_thread = mentions.has_value();
  const int64_t now_ms = util::NowUnixMs();
  int64_t prev_ms = 0;
  std::string run_sender;       // empty: no run open
  std::vector<bool> plain_rows; // parallel to `rows`: a plain message (the only kind that has runs)
  int64_t run_start_ms = 0;     // time of the previous message of the open run
  // #210: each message's send mark, asked once. A 1:1 "did not go out" line goes when its message shows the red mark
  // (the mark tells it, and a tap reads the line's reason); otherwise — a reaction, a deleted or local row, a clock,
  // no mark port — the line is the only sign and stays.
  std::unordered_map<std::string, SendMark> marks;
  if (facts.send_mark) {
    for (const TranscriptRow& entry : entries) {
      if (!entry.orphan_annotation && !entry.control) {
        marks[entry.message.id] = facts.send_mark(entry.message);
      }
    }
  }
  const std::string failure_prefix = SendFailureRowId("", "");
  for (const TranscriptRow& entry : entries) {
    if (const auto reason = SendFailureRowReason(entry.message);
        reason && *reason != kGroupMembersMissedWire && entry.message.id.rfind(failure_prefix, 0) == 0) {
      const auto marked = marks.find(entry.message.id.substr(failure_prefix.size()));
      if (marked != marks.end() && marked->second == SendMark::Failed) {
        continue;
      }
    }
    if (entry.orphan_annotation) {
      rows.push_back(OrphanAnnotationRow(entry, facts));
      plain_rows.push_back(false);
      continue;
    }
    const ThreadMessage& message = entry.message;
    // A call summary: decoded once, its side is the starter's (a group's is written locally).
    const std::optional<CallSummaryRow> call_row = entry.control ? CallSummaryRowOf(message) : std::nullopt;
    bool mentions_me = false;
    std::string content = MessageRml(entry, facts, call_row, mentions, mentions_me);
    if (content.empty()) {
      continue;
    }
    if (entry.asked_question) {
      content = "<div class=\"chat-quote chat-quote--asked\"><p class=\"chat-quote-text\">" +
                StructuredTextParser::EscapeText(OneLine(*entry.asked_question)) + "</p></div>" + content;
    }
    if (!entry.reactions.empty() && !AddReactionsToHost(content, message.id, entry.reactions)) {
      content += ReactionsRml(message.id, entry.reactions, false);
    }
    for (const std::string& badge : entry.annotation_badges) {
      content += "<span class=\"chat-annotation-badge\" title=\"" + StructuredTextParser::EscapeText(badge) + "\">" +
                 StructuredTextParser::EscapeText(badge) + "</span>";
    }
    MessageDisplayRow row;
    row.message_id = message.id.c_str();
    row.display_order = message.display_order;
    row.ai_image_json = message.ai_image_json.value_or(std::string());
    // A feature's control lines already include names; no sender chip above the system line.
    row.sender_label =
        entry.control || !facts.sender_label ? "" : facts.sender_label(message.sender_contact_id).c_str();
    row.content_rml = content.c_str();
    row.row_class = call_row ? (call_row->ours ? "message-row-user" : "message-row-peer")
                             : RowClass(message.sender_contact_id).c_str();
    row.mentions_me = mentions_me;
    row.has_reactions = !entry.reactions.empty();
    if (!entry.control) {
      row.time_label = EditedTimeLabel(message, MessageTimeLabel(message.timestamp, now_ms)).c_str();
    }
    const bool new_day = message.timestamp > 0 && !SameLocalDay(prev_ms, message.timestamp);
    if (new_day) {
      row.date_label = MessageDateLabel(message.timestamp, now_ms).c_str();
    }
    if (message.timestamp > 0) {
      prev_ms = message.timestamp;
    }
    const bool plain = !entry.control && message.content_type != ChatContentType::System;
    const bool continues_run = plain && !new_day && run_sender == message.sender_contact_id &&
                               message.timestamp - run_start_ms <= kRunGapMs && message.timestamp >= run_start_ms;
    row.run_start = !continues_run;
    run_sender = plain ? message.sender_contact_id : std::string();
    run_start_ms = message.timestamp;
    if (group_thread && plain && message.sender_contact_id != kLocalSelfContactId &&
        message.sender_contact_id != kAiAssistantContactId && facts.sender_label) {
      // Every received row keeps the avatar column; the avatar sits beside the last bubble of a run (set below),
      // the name inside the first.
      row.group_column = true;
      const std::string name = facts.sender_label(message.sender_contact_id);
      // Same seed as the Contacts list, the chat list and the call page (the person's id), so one person has one colour.
      const AvatarGlyph glyph = MakeAvatarGlyph(
          name, facts.sender_stable_id ? facts.sender_stable_id(message.sender_contact_id) : std::string());
      row.group_name_tone = glyph.tone;
      row.group_avatar_letter = glyph.letter.c_str();
      if (facts.sender_icon) {
        const std::string icon_path = facts.sender_icon(message.sender_contact_id);
        row.group_has_icon = !icon_path.empty();
        row.group_avatar_src = icon_path.c_str();
      }
      if (row.run_start) {
        row.group_name = name.c_str();
        // The name goes at the top of the bubble; rows without a bubble (cards, calls) keep it as a line above.
        static const std::string kBubbleOpen = "<div class=\"bubble bubble-assistant";
        const size_t bubble_at = content.find(kBubbleOpen);
        const size_t bubble_end = bubble_at == std::string::npos ? bubble_at : content.find('>', bubble_at);
        if (bubble_end != std::string::npos) {
          // A tap on the name opens the member's page (ChatController::OpenGroupSender).
          content.insert(bubble_end + 1, "<div class=\"chat-group-name chat-group-name--" + std::to_string(glyph.tone) +
                                             "\" data-event-click=\"open_sender('" + JsArgInAttribute(message.id) +
                                             "')\">" + StructuredTextParser::EscapeText(name) + "</div>");
          row.content_rml = content.c_str();
          row.group_name_in_bubble = true;
        }
      }
    }
    if (const auto found = marks.find(message.id); found != marks.end() && found->second != SendMark::None) {
      row.send_mark = found->second == SendMark::Queued ? 1 : 2;
      std::string marked = row.content_rml.c_str();
      AddSendMark(marked, found->second, message.id);
      row.content_rml = marked.c_str();
    }
    row.has_content = true;
    rows.push_back(std::move(row));
    plain_rows.push_back(plain);
  }
  // The last bubble of a run carries the tail (and, in groups, the avatar).
  for (size_t i = 0; i < rows.size(); ++i) {
    if (!plain_rows[i] || (i + 1 < rows.size() && !rows[i + 1].run_start)) {
      continue;
    }
    MessageDisplayRow& row = rows[i];
    row.run_end = true;
    row.group_avatar = row.group_column;
    std::string content = row.content_rml.c_str();
    if (AddBubbleTail(content, std::string(row.row_class.c_str()) == "message-row-user")) {
      row.content_rml = content.c_str();
    }
  }
  return rows;
}

} // namespace pbr
