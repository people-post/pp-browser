#pragma once

#include "foundation/crypto/IPskSessionStore.h"
#include "common/thread/IThreadStore.h"
#include "common/thread/ThreadTypes.h"

#include "common/Error.h"
#include "common/PbrCompat.h"

#include <functional>
#include <string>
#include <vector>

namespace pbr {

class IPeerSigningKeyResolver;

/** D060 responder — serve signed outbound envelopes from local thread.db. */
class ChatHistoryResponder {
public:
  using SignBytesFn = std::function<Roe<std::string>(const std::vector<uint8_t>& sign_bytes)>;

  /**
   * @param local_account_id Communicating id for E2E AAD / envelope sender_contact_id
   *        (account id when set; callers may pass relay id as fallback).
   * @param local_device This device's PeerId: the served envelopes are its account copies (M021).
   * @param sign_bytes Signs EnvelopeSigner::BuildSignBytes output with this device's key.
   * @param remote_peer_id The authenticated Amp peer the request came from: the request must be
   *        signed by its `requester_account_id` for this peer (PeerRequestAuth); that account's
   *        thread is served.
   */
  static Roe<ChatHistoryResponse> Serve(IThreadStore& store, IPskSessionStore& psk_store,
                                        const ChatHistoryRequest& request, const std::string& local_relay_user_id,
                                        const std::string& local_account_id, const std::string& local_device,
                                        const SignBytesFn& sign_bytes,
                                        const std::string& remote_peer_id, IPeerSigningKeyResolver& signing_keys,
                                        int64_t now_ms);
};

} // namespace pbr
