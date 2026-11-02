#pragma once

#include <string>
#include <vector>

namespace pbr {

// pp-browser's L4 protocols on the mesh's links (docs/contracts/L4_PROTOCOL_KINDS.md): wire ids, one list for
// every capability that serves or counts them. The capabilities name them in their own words too
// (kDirectChatProtocolId, kChatBlobProtocolId, kCallMediaDirectProtocolId…); the mesh itself never names them.

/** rpc-kind live chat — separate OPEN from history (handler ownership). */
inline constexpr const char* kRpcChatProtocolId = "/pp/rpc/chat/1.0.0";
/** rpc-kind peer history sync (D060) — separate OPEN from live chat. */
inline constexpr const char* kRpcHistoryProtocolId = "/pp/rpc/history/1.0.0";
/** rpc-kind peer-announce tips — separate OPEN from chat/history (Spine B). */
inline constexpr const char* kRpcPeerAnnounceProtocolId = "/pp/rpc/peer-announce/1.0.0";
/** rpc-kind live broadcast ticket + ladder control (Spine F). */
inline constexpr const char* kRpcBroadcastProtocolId = "/pp/rpc/broadcast/1.0.0";
/** blob-kind peer-direct attachment blobs (R019). */
inline constexpr const char* kBlobProtocolId = "/pp/blob/1.0.0";
/** realtime-kind call media (domain/call_media). */
inline constexpr const char* kRealtimeProtocolId = "/pp/realtime/1.0.0";
/** Asking a device for its account's card (docs/contracts/ACCOUNT_CARD.md#card-exchange-over-amp). */
inline constexpr const char* kAccountCardProtocolId = "/pp/account-card/1.0.0";

/**
 * The protocols pp-browser serves on its mesh's links — direct chat, chat history, chat blobs, peer
 * announce, broadcast RPC, call media and account cards — advertised beside the mesh's own
 * (MeshHost::SetAppProtocols). pp-node serves none of them.
 */
inline std::vector<std::string> ProductMeshProtocols() {
  return {kRpcChatProtocolId,         kRpcHistoryProtocolId,   kBlobProtocolId,       kRpcPeerAnnounceProtocolId,
          kRpcBroadcastProtocolId,    kRealtimeProtocolId,     kAccountCardProtocolId};
}

} // namespace pbr
