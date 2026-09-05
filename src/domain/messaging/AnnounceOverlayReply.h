#pragma once

#include "domain/messaging/AnnounceDmReply.h"
#include "domain/messaging/PeerAnnouncePublisher.h"
#include "domain/messaging/PeerAnnounceTypes.h"

#include "common/Error.h"

#include <cstdint>
#include <string>
#include <string_view>

#include "common/PbrCompat.h"

namespace pbr {

/** Viewer request for on-screen overlay (still a DM/rpc to the publisher). */
struct AnnounceOverlayReplyPlan {
  AnnounceDmReplyPlan dm;
  std::string join_handle;
  std::string viewer_msg_id;
  std::string text;
  /** Wire body for SendUserMessage / rpc. */
  std::string message_body;
};

/**
 * Plan an overlay reply: DM target + length-capped text + opaque viewer_msg_id.
 * Does not speak on the announce topic; publisher must rebroadcast as live_chat.
 */
Roe<AnnounceOverlayReplyPlan> PlanAnnounceOverlayReply(std::string_view tip_peer_id,
                                                       std::string_view contact_id,
                                                       std::string_view contact_account_id,
                                                       std::string_view thread_title,
                                                       std::string_view join_handle,
                                                       std::string_view text,
                                                       std::string_view viewer_msg_id);

Roe<std::string> EncodeAnnounceOverlayReplyBody(std::string_view join_handle, std::string_view viewer_msg_id,
                                                std::string_view text);

struct AnnounceOverlayReplyBody {
  std::string join_handle;
  std::string viewer_msg_id;
  std::string text;
};

Roe<AnnounceOverlayReplyBody> DecodeAnnounceOverlayReplyBody(std::string_view message_body);

/** Simple per-viewer / per-publisher rate gates (in-memory helpers). */
bool AnnounceOverlayViewerAllowed(int64_t last_request_ms, int64_t now_ms);
bool AnnounceOverlayPublisherAllowed(int emitted_in_window, int64_t window_start_ms, int64_t now_ms);

/**
 * Build a publisher Draft for a signed live_chat tip from a decoded overlay request.
 * Caller supplies topic/program (from the live tip) and viewer_peer_id.
 */
Roe<PeerAnnouncePublisher::Draft> MakeLiveChatAnnounceDraft(std::string_view topic_id,
                                                            std::string_view program_id,
                                                            std::string_view join_handle,
                                                            std::string_view viewer_peer_id,
                                                            const AnnounceOverlayReplyBody& body);

} // namespace pbr
