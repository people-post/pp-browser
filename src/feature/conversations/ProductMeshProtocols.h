#pragma once

#include "common/chat/IDirectMessageClient.h"
#include "common/media/CallMediaProtocol.h"
#include "common/thread/ChatBlobTypes.h"
#include "lib/messaging/AccountCardRequest.h"

#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * The protocols pp-browser serves on its mesh's links — direct chat, chat history, chat blobs, peer
 * announce, broadcast RPC, call media and account cards — advertised beside the mesh's own
 * (MeshHost::SetAppProtocols). pp-node serves none of them.
 */
inline std::vector<std::string> ProductMeshProtocols() {
  return {kDirectChatProtocolId,      kChatHistoryProtocolId,  kChatBlobProtocolId,
          kRpcPeerAnnounceProtocolId, kRpcBroadcastProtocolId, kCallMediaDirectProtocolId,
          kAccountCardProtocolId};
}

} // namespace pbr
