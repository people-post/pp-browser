#pragma once

#include "common/Error.h"
#include "common/Module.h"
#include "domain/messaging/CallMediaKeyStore.h"
#include "domain/messaging/CallSessionStore.h"
#include "feature/calls/CallControlClient.h"

#include <cstdint>
#include <functional>
#include <string>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * A call's media epoch keys between the peers: mint (a new call, a rotation), wrap into an invite,
 * send as CallMediaKey (on accept, on resend, after a rotation), and take a peer's wrapped key in
 * (invite or CallMediaKey) — stored, then "key ready" so a 1:1 start waiting on it (V015) goes on.
 * Keys are wrapped under the peer's E2E session key. The call session workflow decides when; this
 * does the key work. Calls owner.
 */
class CallMediaKeyExchange : public Module {
public:
  /** One epoch key of a call. */
  struct EpochKey {
    uint32_t epoch = 0;
    std::string key_id;
    ByteVector key;
  };
  using KeyReadyFn = std::function<void(const std::string& call_id)>;

  CallMediaKeyExchange(CallSessionStore& sessions, CallMediaKeyStore& keys, CallControlClient& control);

  /** Runs after a peer's key is stored (the 1:1 start may be waiting for it). */
  void SetOnKeyReady(KeyReadyFn fn) { on_key_ready_ = std::move(fn); }

  /** A new key for `call_id` at `epoch`, stored. The caller records epoch / key id on the session. */
  Roe<EpochKey> Mint(const std::string& call_id, uint32_t epoch);
  /** The next epoch's key, stored and recorded on the call's session. */
  Roe<EpochKey> Rotate(const std::string& call_id);
  /** The call's current key (per its session). */
  Roe<EpochKey> Current(const std::string& call_id) const;

  /** `key` wrapped for `peer_identity` — embedded in the invite; empty when it cannot be wrapped. */
  std::string WrapForPeer(const std::string& call_id, const EpochKey& key, const std::string& peer_identity);

  /** Send `key` to `peer_identity` as CallMediaKey. */
  Roe<void> Send(const std::string& call_id, const std::string& peer_identity, const EpochKey& key);
  /** Send the call's current key to `peer_identity`. */
  Roe<void> SendCurrent(const std::string& call_id, const std::string& peer_identity);

  /**
   * Take the key `sender_identity` wrapped for us (`what`: where it came, for the log). Stored:
   * key ready runs, and true.
   */
  bool TakeWrapped(const std::string& call_id, uint32_t epoch, const std::string& key_id,
                   const std::string& wrapped_key_b64, const std::string& sender_identity, const char* what);

  /** An inbound CallMediaKey: the session moves to its epoch; its key is taken. */
  Roe<void> HandleInbound(const std::string& detail_json, const std::string& sender_identity);

private:
  CallSessionStore& sessions_;
  CallMediaKeyStore& keys_;
  CallControlClient& control_;
  KeyReadyFn on_key_ready_;
};

} // namespace pbr
