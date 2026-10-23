#pragma once

#include "common/Error.h"
#include "common/thread/ChatBlobTypes.h"
#include "common/thread/ChatHistoryTypes.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

class IPeerSigningKeyResolver;

/**
 * Peer-direct (Amp) history / chat-blob requests name who asks. The Amp link authenticates a device
 * (its PeerId), not an account: the requester signs the request with its account key, over its
 * fields, its own device PeerId and the time. The responder serves the signer's thread only, and only
 * on the link of the PeerId it signed (a copied request does not work from another peer).
 */
namespace PeerRequestAuth {

/** How far a request's signed time may be from ours. */
inline constexpr int64_t kMaxSkewMs = 5 * 60 * 1000;

Roe<std::vector<uint8_t>> SignBytes(const ChatHistoryRequest& request);
Roe<std::vector<uint8_t>> SignBytes(const ChatBlobRequest& request);

using SignFn = std::function<Roe<std::string>(const std::vector<uint8_t>&)>;

/** This device's Amp PeerId (its mesh identity is the device key). */
Roe<std::string> DevicePeerId(const std::string& device_public_key_b64);

/** Fill the auth fields and sign, as `account_id` on the link of `local_peer_id`. */
Roe<void> Sign(ChatHistoryRequest& request, const std::string& account_id, const std::string& local_peer_id,
               int64_t now_ms, const SignFn& sign);
Roe<void> Sign(ChatBlobRequest& request, const std::string& account_id, const std::string& local_peer_id,
               int64_t now_ms, const SignFn& sign);

/** Verified, from `remote_peer_id`, recent: the request's `requester_account_id` may be trusted. */
Roe<void> Verify(const ChatHistoryRequest& request, const std::string& remote_peer_id, int64_t now_ms,
                 IPeerSigningKeyResolver& keys);
Roe<void> Verify(const ChatBlobRequest& request, const std::string& remote_peer_id, int64_t now_ms,
                 IPeerSigningKeyResolver& keys);

} // namespace PeerRequestAuth

} // namespace pbr
