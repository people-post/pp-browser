#include "domain/messaging/AnnounceOverlayReply.h"

#include "common/ValueJson.h"

namespace pbr {
namespace {

std::string TrimCopy(std::string_view in) {
  size_t b = 0;
  while (b < in.size() && (in[b] == ' ' || in[b] == '\t' || in[b] == '\n' || in[b] == '\r')) {
    ++b;
  }
  size_t e = in.size();
  while (e > b && (in[e - 1] == ' ' || in[e - 1] == '\t' || in[e - 1] == '\n' || in[e - 1] == '\r')) {
    --e;
  }
  return std::string(in.substr(b, e - b));
}

} // namespace

Roe<std::string> EncodeAnnounceOverlayReplyBody(const std::string_view join_handle,
                                                const std::string_view viewer_msg_id,
                                                const std::string_view text) {
  if (join_handle.empty() || viewer_msg_id.empty()) {
    return Error("overlay reply requires join_handle and viewer_msg_id");
  }
  const std::string trimmed = TrimCopy(text);
  if (trimmed.empty()) {
    return Error("overlay reply text empty");
  }
  if (trimmed.size() > kAnnounceOverlayMaxBodyChars) {
    return Error("overlay reply text too long");
  }
  Object json;
  json.set("intent", kAnnounceOverlayIntent);
  json.set("join_handle", std::string(join_handle));
  json.set("viewer_msg_id", std::string(viewer_msg_id));
  json.set("text", trimmed);
  return DumpJson(json);
}

Roe<AnnounceOverlayReplyBody> DecodeAnnounceOverlayReplyBody(const std::string_view message_body) {
  auto parsed = ParseObject(std::string(message_body));
  if (!parsed) {
    return parsed.error();
  }
  const Object& o = *parsed;
  if (ObjectString(o, "intent").value_or("") != kAnnounceOverlayIntent) {
    return Error("not an announce overlay reply body");
  }
  AnnounceOverlayReplyBody body;
  body.join_handle = ObjectString(o, "join_handle").value_or("");
  body.viewer_msg_id = ObjectString(o, "viewer_msg_id").value_or("");
  body.text = TrimCopy(ObjectString(o, "text").value_or(""));
  if (body.join_handle.empty() || body.viewer_msg_id.empty() || body.text.empty()) {
    return Error("overlay reply body missing fields");
  }
  if (body.text.size() > kAnnounceOverlayMaxBodyChars) {
    return Error("overlay reply text too long");
  }
  return body;
}

Roe<AnnounceOverlayReplyPlan> PlanAnnounceOverlayReply(const std::string_view tip_peer_id,
                                                       const std::string_view contact_id,
                                                       const std::string_view contact_account_id,
                                                       const std::string_view thread_title,
                                                       const std::string_view join_handle,
                                                       const std::string_view text,
                                                       const std::string_view viewer_msg_id) {
  if (join_handle.empty()) {
    return Error("overlay reply requires join_handle");
  }
  if (viewer_msg_id.empty()) {
    return Error("overlay reply requires viewer_msg_id");
  }
  auto dm = PlanAnnounceDmReply(tip_peer_id, contact_id, contact_account_id, thread_title);
  if (!dm) {
    return dm.error();
  }
  auto encoded = EncodeAnnounceOverlayReplyBody(join_handle, viewer_msg_id, text);
  if (!encoded) {
    return encoded.error();
  }
  AnnounceOverlayReplyPlan plan;
  plan.dm = std::move(*dm);
  plan.join_handle = std::string(join_handle);
  plan.viewer_msg_id = std::string(viewer_msg_id);
  plan.text = TrimCopy(text);
  plan.message_body = std::move(*encoded);
  return plan;
}

bool AnnounceOverlayViewerAllowed(const int64_t last_request_ms, const int64_t now_ms) {
  if (last_request_ms <= 0) {
    return true;
  }
  return now_ms - last_request_ms >= kAnnounceOverlayViewerMinIntervalMs;
}

bool AnnounceOverlayPublisherAllowed(const int emitted_in_window, const int64_t window_start_ms,
                                     const int64_t now_ms) {
  if (window_start_ms <= 0 || now_ms - window_start_ms >= 60'000) {
    return true;
  }
  return emitted_in_window < static_cast<int>(kAnnounceOverlayPublisherMaxPerMinute);
}

Roe<PeerAnnouncePublisher::Draft> MakeLiveChatAnnounceDraft(const std::string_view topic_id,
                                                            const std::string_view program_id,
                                                            const std::string_view join_handle,
                                                            const std::string_view viewer_peer_id,
                                                            const AnnounceOverlayReplyBody& body) {
  if (topic_id.empty() || program_id.empty()) {
    return Error("live_chat draft requires topic_id and program_id");
  }
  if (join_handle.empty() || join_handle != body.join_handle) {
    return Error("live_chat draft join_handle mismatch");
  }
  if (body.viewer_msg_id.empty() || body.text.empty()) {
    return Error("live_chat draft missing overlay fields");
  }
  if (viewer_peer_id.empty()) {
    return Error("live_chat draft requires viewer_peer_id");
  }
  PeerAnnouncePublisher::Draft draft;
  draft.topic_id = std::string(topic_id);
  draft.program_id = std::string(program_id);
  draft.state = PeerAnnounceState::Live;
  draft.join_handle = body.join_handle;
  draft.kind = kPeerAnnounceKindLiveChat;
  draft.viewer_peer_id = std::string(viewer_peer_id);
  draft.viewer_msg_id = body.viewer_msg_id;
  draft.body = body.text;
  return draft;
}

} // namespace pbr
