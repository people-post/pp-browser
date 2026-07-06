#pragma once

#include "base/crypto/CryptoTypes.h"
#include "base/crypto/IPskSessionStore.h"
#include "base/messaging/ThreadTypes.h"

#include "common/Error.h"

#include <cstdint>
#include <string>

namespace pbr {

struct E2eEncryptParams {
  std::string text;
  CryptoChannel channel = CryptoChannel::E2e;
  std::string peer_contact_id;
  std::string sender_contact_id;
  std::string message_id;
  uint64_t sender_seq = 0;
  uint32_t session_epoch = 1;
  int64_t timestamp = 0;
};

struct E2eEncryptResult {
  std::string payload_b64;
  std::optional<std::string> key_init_b64;
};

/** E2E ciphertext in body.e2e.payload_b64 (D090) — base/messaging wire codec. */
class E2eRelayPayloadCodec {
public:
  static CryptoChannel ChannelFromThread(ThreadChannel channel);
  static ChatTargetKey ChatTargetFromThread(const Thread& thread);
  static bool RequiresEncryption(ThreadChannel channel);

  static Roe<std::string> EncryptText(const E2eEncryptParams& params, const ByteVector& master_psk);
  static Roe<E2eEncryptResult> EncryptTextWithAutoKey(const E2eEncryptParams& params, const ByteVector& master_psk,
                                                      const std::optional<std::string>& key_init_b64 = std::nullopt);
  static Roe<ThreadMessage> DecryptEnvelope(const RelayEnvelope& envelope, const std::string& local_contact_id,
                                            const ChatTargetKey& target_key, IPskSessionStore& psk_store,
                                            const std::optional<ByteVector>& local_kem_private_key = std::nullopt);
};

} // namespace pbr
