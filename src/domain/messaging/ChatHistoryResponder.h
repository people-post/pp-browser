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

/** D060 responder — serve signed outbound envelopes from local thread.db. */
class ChatHistoryResponder {
public:
  using SignBytesFn = std::function<Roe<std::string>(const std::vector<uint8_t>& sign_bytes)>;

  /**
   * @param local_account_id Communicating id for E2E AAD / envelope sender_contact_id
   *        (account id when set; callers may pass relay id as fallback).
   * @param sign_bytes Signs EnvelopeSigner::BuildSignBytes output (ML-DSA / Ed25519 via identity).
   */
  static Roe<ChatHistoryResponse> Serve(IThreadStore& store, IPskSessionStore& psk_store,
                                        const ChatHistoryRequest& request, const std::string& local_relay_user_id,
                                        const std::string& local_account_id, const SignBytesFn& sign_bytes);
};

} // namespace pbr
