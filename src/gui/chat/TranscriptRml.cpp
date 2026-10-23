#include "gui/chat/TranscriptRml.h"

#include "gui/chat/DeliveryNoticeText.h"
#include "common/ValueJson.h"

#include "domain/ai/StructuredTextParser.h"
#include "domain/messaging/AttachmentCache.h"
#include "domain/messaging/ChatPayloadCodec.h"
#include "domain/messaging/GroupMembershipCodec.h"
#include "domain/messaging/PlainTextLinks.h"
#include "domain/messaging/PskRotateCodec.h"
#include "domain/messaging/QuoteReply.h"
#include "domain/ui/ChatFormHelper.h"
#include "common/chat/MessagingJson.h"
#include "common/ui/WorkingSetCodec.h"
#include "feature/conversations/AttachmentContent.h"
#include "foundation/i18n/LocalizationService.h"

#include <algorithm>
#include <cctype>
#include <sstream>

#include "common/PbrCompat.h"

namespace pbr {
namespace {

std::string InlineChatActionButtonsRml(const std::vector<TranscriptChatAction>& chat_actions) {
  std::ostringstream out;
  for (size_t i = 0; i < chat_actions.size(); ++i) {
    out << "<button class=\"chat-suggestion\" data-event-click=\"send_chat_action('__ENTRY__', " << i << ")\">"
        << StructuredTextParser::EscapeText(chat_actions[i].label) << "</button>";
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
  return ReceiveFailureText(*reason, detail->getString("sender").value_or(""), detail->getString("family").value_or(""));
}

std::string SystemRml(const TranscriptRow& entry, const TranscriptFacts& facts) {
  const ThreadMessage& message = entry.message;
  if (entry.control) {
    // A feature's message: its own line, names included, else the message's text.
    const auto line = facts.control_line ? facts.control_line(message) : std::nullopt;
    return SystemLineRml(line.value_or(message.text));
  }

  const auto control_type = GroupMembershipCodec::ControlTypeFromMessage(message);
  if (control_type && *control_type == GroupMembershipControlType::GroupInvite) {
    if (GroupMembershipCodec::InviteResolutionFromMessage(message)) {
      return SystemLineRml(message.text);
    }
    auto invite = GroupMembershipCodec::DecodeInviteFromMessage(message);
    const std::string title =
        invite ? invite->group_title : (message.text.empty() ? "Group invitation" : message.text);
    std::string html = "<div class=\"chat-card chat-group-invite\"><h3 class=\"heading-3\">" +
                       StructuredTextParser::EscapeText(title) + "</h3>";
    html += "<p class=\"text muted\">" + StructuredTextParser::EscapeText(message.text) + "</p>";
    html += HydrateChatActions("", message.chat_actions);
    html += "</div>";
    if (!message.chat_actions.empty() && html.find("__ENTRY__") != std::string::npos) {
      return InjectEntryPlaceholders(html, JsArgInAttribute(message.id));
    }
    return html;
  }
  if (control_type && *control_type == GroupMembershipControlType::GroupOwnerUnreachable) {
    if (GroupMembershipCodec::IsOwnerUnreachableResolved(message)) {
      return SystemLineRml(message.text);
    }
    const std::string body =
        "You can keep chatting with people who are online. Inviting, renaming for everyone, and removing "
        "members need the owner. If they’ve left for good, start a new group from this one.";
    std::string html = "<div class=\"chat-card chat-group-invite\"><h3 class=\"heading-3\">" +
                       StructuredTextParser::EscapeText("Owner hasn’t been reachable") + "</h3>";
    html += "<p class=\"text muted\">" + StructuredTextParser::EscapeText(body) + "</p>";
    html += HydrateChatActions("", message.chat_actions);
    html += "</div>";
    if (!message.chat_actions.empty() && html.find("__ENTRY__") != std::string::npos) {
      return InjectEntryPlaceholders(html, JsArgInAttribute(message.id));
    }
    return html;
  }
  if (auto failure = ReceiveFailureLine(message)) {
    return SystemLineRml(*failure);
  }
  if (PskRotateCodec::IsPskRotateMessage(message) && message.sender_contact_id != kLocalSelfContactId) {
    return SystemLineRml("They're using only this device for new messages.");
  }
  return SystemLineRml(message.text);
}

std::string ContactCardRml(const ThreadMessage& message) {
  auto fields = ChatPayloadCodec::DecodeContactCardJson(message.payload_json);
  const std::string name =
      fields ? fields->display_name : (message.text.empty() ? "Contact" : message.text);
  const std::string relay = fields && !fields->relay_user_id.empty() ? fields->relay_user_id : "";
  std::string html = "<div class=\"chat-card chat-contact-card\">";
  // The card's avatar_url is a peer-supplied string; it is never used as a local image path.
  html += "<h3 class=\"heading-3\">" + StructuredTextParser::EscapeText(name) + "</h3>";
  if (!relay.empty()) {
    html += "<p class=\"text muted\">" + StructuredTextParser::EscapeText(relay) + "</p>";
  }
  html += "</div>";
  return html;
}

std::string CryptoTxRml(const ThreadMessage& message) {
  auto fields = ChatPayloadCodec::DecodeCryptoTxJson(message.payload_json);
  const std::string summary = message.text.empty() ? "Transaction" : message.text;
  std::string html = "<div class=\"chat-card chat-tx-card\"><h3 class=\"heading-3\">" +
                     StructuredTextParser::EscapeText(summary) + "</h3>";
  if (fields) {
    html += "<p class=\"text muted\">" + StructuredTextParser::EscapeText(fields->asset) + " · " +
            StructuredTextParser::EscapeText(fields->amount) + " · " +
            StructuredTextParser::EscapeText(fields->direction) + "</p>";
    if (!fields->status.empty()) {
      html += "<p class=\"text muted\">Status: " + StructuredTextParser::EscapeText(fields->status) + "</p>";
    }
  }
  html += "</div>";
  return html;
}

std::string AttachmentRml(const ThreadMessage& message, const TranscriptFacts& facts) {
  auto fields = ChatPayloadCodec::DecodeAttachmentJson(message.payload_json);
  const std::string label = fields && !fields->filename.empty()
                                ? fields->filename
                                : (message.text.empty() ? "Attachment" : message.text);

  const TranscriptAttachmentView view =
      fields && facts.attachment ? facts.attachment(message) : TranscriptAttachmentView{};
  const AttachmentFetchWorkflow::DownloadState state = view.state;
  const std::string& local_path = view.local_path;

  std::string html = "<div class=\"chat-card chat-attachment-card\">";
  const bool ready = fields && state == AttachmentFetchWorkflow::DownloadState::Ready;
  if (ready && IsAttachmentImageMime(fields->mime) && !local_path.empty()) {
    html += "<img class=\"chat-attachment-image\" src=\"" + StructuredTextParser::EscapeText(local_path) + "\"/>";
  } else if (ready && IsAttachmentVideoMime(fields->mime)) {
    html += "<div class=\"chat-attachment-video\">";
    const std::string& poster_path = view.poster_path;
    if (!poster_path.empty()) {
      html += "<img class=\"chat-attachment-video-poster\" src=\"" +
              StructuredTextParser::EscapeText(poster_path) + "\"/>";
    } else {
      html += "<div class=\"chat-attachment-video-badge\">Video</div>";
      html += "<p class=\"text\">" + StructuredTextParser::EscapeText(label) + "</p>";
    }
    if (fields->byte_length > 0) {
      html += "<p class=\"text muted\">" + StructuredTextParser::EscapeText(FormatAttachmentByteSize(fields->byte_length)) +
              "</p>";
    }
    html += "<button class=\"chat-suggestion\" type=\"button\" data-event-click=\"open_attachment('" +
            JsArgInAttribute(message.id) + "')\">" + Tr("chat.attachment.open") + "</button>";
    html += "</div>";
  } else if (ready && !local_path.empty()) {
    html += "<h3 class=\"heading-3\">" + StructuredTextParser::EscapeText(label) + "</h3>";
    if (fields->byte_length > 0) {
      html += "<p class=\"text muted\">" + StructuredTextParser::EscapeText(FormatAttachmentByteSize(fields->byte_length)) +
              "</p>";
    }
    html += "<button class=\"chat-suggestion\" type=\"button\" data-event-click=\"open_attachment('" +
            JsArgInAttribute(message.id) + "')\">" + Tr("chat.attachment.open") + "</button>";
  } else {
    html += "<h3 class=\"heading-3\">" + StructuredTextParser::EscapeText(label) + "</h3>";
    if (fields && fields->byte_length > 0) {
      html += "<p class=\"text muted\">" + StructuredTextParser::EscapeText(FormatAttachmentByteSize(fields->byte_length)) +
              "</p>";
    }
    if (state == AttachmentFetchWorkflow::DownloadState::Downloading) {
      html += "<p class=\"text muted\">" + Tr("chat.attachment.downloading") + "</p>";
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
  return "<div class=\"chat-card chat-unsupported-card\"><p class=\"text muted\">Update to view this "
         "message.</p></div>";
}

// Escaped text with its https links tappable. The click carries the message id and the link's index;
// the handler finds the URL again in the stored text (PlainTextLinks.h), so none sits in the markup.
// `text` must be the message text or a prefix of it, so the indexes agree.
std::string LinkedTextRml(const std::string& text, const std::string& id_arg) {
  const std::vector<PlainTextLink> links = FindPlainTextLinks(text);
  if (links.empty() || id_arg.empty()) {
    return StructuredTextParser::EscapeText(text);
  }
  std::string out;
  size_t pos = 0;
  for (size_t i = 0; i < links.size(); ++i) {
    out += StructuredTextParser::EscapeText(text.substr(pos, links[i].begin - pos));
    out += "<span class=\"chat-link\" data-event-click=\"open_message_link('" + id_arg + "', " + std::to_string(i) +
           ")\">" + StructuredTextParser::EscapeText(links[i].display) + "</span>";
    pos = links[i].end;
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

std::string MessageRml(const TranscriptRow& entry, const TranscriptFacts& facts) {
  const ThreadMessage& message = entry.message;
  if (message.content_rml) {
    std::string rml = *message.content_rml;
    if (rml.find("__ENTRY__") != std::string::npos) {
      rml = InjectEntryPlaceholders(rml, JsArgInAttribute(message.id));
    }
    const bool has_snapshot = message.working_set_json && !message.working_set_json->empty() &&
                              !WorkingSetCandidatesFromJson(*message.working_set_json).empty();
    // Legacy bug: panel row actions were dumped into the bubble as chat-suggestion chips.
    if (ContentRmlHasWorkingSetChip(rml) || has_snapshot) {
      rml = StripInlinedWorkingSetActionSuggestions(rml);
    }
    if (ContentRmlHasActiveWorkingSetChip(rml) && !has_snapshot) {
      rml = MarkWorkingSetChipsUnavailable(rml);
    }
    return rml;
  }
  if (message.content_type == ChatContentType::System) {
    return SystemRml(entry, facts);
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
  const std::string bubble_class = message.sender_contact_id == kLocalSelfContactId ? "bubble-user" : "bubble-assistant";
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
  std::string text_rml;
  if (const auto parts = SplitQuoteReply(message.text)) {
    text_rml = "<div class=\"chat-quote chat-quote--jump\" data-event-click=\"jump_to_quote('" + id_arg +
               "')\"><p class=\"chat-quote-text\">" + StructuredTextParser::EscapeText(OneLine(parts->quote)) + "</p></div>" +
               paragraph + LinkedTextRml(parts->reply, id_arg) + "</p>";
  } else {
    text_rml = paragraph + LinkedTextRml(message.text, id_arg) + "</p>";
  }
  std::string body = badges + "<div class=\"bubble " + bubble_class + "\" selectable=\"text\">" + text_rml + "</div>";
  body = HydrateChatActions(body, message.chat_actions);
  // __ENTRY__ is only ever introduced by HydrateChatActions' own template above; never treat a
  // literal "__ENTRY__" typed into message.text (peer-controlled) as a placeholder to fill in.
  if (!message.chat_actions.empty() && body.find("__ENTRY__") != std::string::npos) {
    return InjectEntryPlaceholders(body, JsArgInAttribute(message.id));
  }
  return body;
}


std::string ReactionsRml(const std::string& target_id, const std::vector<TranscriptReaction>& reactions) {
  std::ostringstream rml;
  // menu-anchor-end: the message menu opens right under the bubble, over this row (ContextMenuHost).
  rml << "<div class=\"chat-reaction-row\" menu-anchor-end=\"\">";
  for (const TranscriptReaction& chip : reactions) {
    rml << "<button class=\"chat-reaction-chip";
    if (chip.mine) {
      rml << " chat-reaction-chip--mine";
    }
    rml << "\" type=\"button\" data-event-click=\"toggle_reaction('" << JsArgInAttribute(target_id) << "', '"
        << JsArgInAttribute(chip.emoji) << "')\">" << StructuredTextParser::EscapeText(chip.emoji);
    if (chip.count > 1) {
      rml << " " << chip.count;
    }
    rml << "</button>";
  }
  rml << "</div>";
  return rml.str();
}

MessageDisplayRow OrphanAnnotationRow(const TranscriptRow& entry, const TranscriptFacts& facts) {
  const ThreadMessage& annotation = entry.message;
  MessageDisplayRow row;
  row.message_id = annotation.id.c_str();
  row.display_order = annotation.display_order;
  row.sender_label = facts.sender_label ? facts.sender_label(annotation.sender_contact_id).c_str() : "";
  const std::string text = annotation.text.empty() ? "?" : annotation.text;
  row.content_rml = ("<div class=\"chat-orphan-annotation muted\"><span class=\"chat-annotation-badge\">" +
                     StructuredTextParser::EscapeText(text) + "</span> <span class=\"text-xs\">(unlinked)</span></div>")
                        .c_str();
  row.row_class = "message-row-annotation-orphan";
  row.has_content = true;
  return row;
}

} // namespace

std::vector<MessageDisplayRow> RenderTranscript(const std::vector<TranscriptRow>& entries,
                                                const TranscriptFacts& facts) {
  std::vector<MessageDisplayRow> rows;
  rows.reserve(entries.size());
  for (const TranscriptRow& entry : entries) {
    if (entry.orphan_annotation) {
      rows.push_back(OrphanAnnotationRow(entry, facts));
      continue;
    }
    const ThreadMessage& message = entry.message;
    std::string content = MessageRml(entry, facts);
    if (content.empty()) {
      continue;
    }
    if (entry.asked_question) {
      content = "<div class=\"chat-quote chat-quote--asked\"><p class=\"chat-quote-text\">" +
                StructuredTextParser::EscapeText(OneLine(*entry.asked_question)) + "</p></div>" + content;
    }
    if (!entry.reactions.empty()) {
      content += ReactionsRml(message.id, entry.reactions);
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
    row.row_class = RowClass(message.sender_contact_id).c_str();
    // Local is expected for AI/system-local rows — skip it to keep meta quiet.
    if (!entry.control && message.transport && *message.transport != MessageTransport::Local) {
      row.transport_badge = MessageTransportBadgeLabel(*message.transport).c_str();
    }
    row.has_content = true;
    rows.push_back(std::move(row));
  }
  return rows;
}

} // namespace pbr
