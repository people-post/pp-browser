#pragma once

#include "common/Error.h"
#include "common/Module.h"
#include "common/thread/IThreadStore.h"
#include "lib/calls/CallControlCodec.h"
#include "lib/calls/CallSessionStore.h"
#include "lib/people/ContactsStore.h"
#include "lib/people/IdentityStore.h"
#include "feature/calls/CallDeliveryPorts.h"
#include "foundation/crypto/IPskSessionStore.h"

#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * Outbound call-control I/O: signaling to a peer on the control lane, fan-out to a call's
 * participants, the media key wrapped for a peer, and the call's summary in its thread. The call
 * session manager decides what to send; this sends it. Calls owner.
 */
class CallControlClient : public Module {
public:
  CallControlClient(IThreadStore& store, IdentityStore& identity, CallSessionStore& sessions,
                    IPskSessionStore& psk_store, const CallDeliveryPorts& delivery);

  Roe<std::string> LocalRelayIdentity() const;
  /** Signaling to `peer_identity` on the control lane. */
  Roe<void> SendDirect(const std::string& peer_identity, CallControlType type, const std::string& detail_json,
                       const std::string& display);
  /**
   * The call's summary into `thread_id`'s chat stream (both sides, all their devices read it); kept
   * here only when it cannot be sent (an unverified private key, no route) or `local_only`.
   */
  Roe<void> SendSummary(const std::string& thread_id, const CallSummaryDetail& summary, bool local_only = false);
  /** Best-effort to every joined participant but `skip_identity`. */
  Roe<void> FanOutToJoined(const std::string& call_id, CallControlType type, const std::string& detail_json,
                           const std::string& display, const std::string& skip_identity);
  /** Also to ringing / invited participants — ending a call so invitees clear their ring. */
  Roe<void> FanOutToJoinedAndRinging(const std::string& call_id, CallControlType type,
                                     const std::string& detail_json, const std::string& display,
                                     const std::string& skip_identity);
  /** The call's media key, wrapped under the peer's E2E session key. */
  Roe<void> SendMediaKey(const std::string& call_id, const std::string& peer_identity, uint32_t media_epoch,
                         const std::string& media_key_id, const ByteVector& key_bytes);

  /** The E2E session key with `peer_identity` (ensured when missing) — media keys are wrapped under it. */
  Roe<ByteVector> ResolvePeerSessionKey(const std::string& peer_identity) const;

private:
  IThreadStore& store_;
  IdentityStore& identity_;
  CallSessionStore& sessions_;
  IPskSessionStore& psk_store_;
  const CallDeliveryPorts& delivery_;
  /**
   * AutoKey key_init from ensure_peer_session_key — rides the next SendDirect to that peer (the
   * peer needs it to derive the session key the media key is wrapped under).
   */
  mutable std::mutex pending_key_init_mutex_;
  mutable std::unordered_map<std::string, std::string> pending_key_init_;
};

} // namespace pbr
