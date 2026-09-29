#pragma once

#include "common/Error.h"
#include "common/Module.h"
#include "common/thread/IThreadStore.h"
#include "domain/messaging/CallControlCodec.h"
#include "domain/messaging/CallSessionStore.h"
#include "domain/people/ContactsStore.h"
#include "domain/people/IdentityStore.h"
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
 * Outbound call-control I/O: call-control messages to a peer (over its E2E direct thread), fan-out
 * to a call's participants, the call's origin-thread history, and the media key wrapped for a peer.
 * The call session manager decides what to send; this sends it. Calls owner.
 */
class CallControlClient : public Module {
public:
  /** The thread the active call came from (call-control prefers it for its peer); empty if none. */
  using PreferredThreadFn = std::function<std::optional<std::string>()>;

  CallControlClient(IThreadStore& store, ContactsStore& contacts, IdentityStore& identity,
                    CallSessionStore& sessions, IPskSessionStore& psk_store, const CallDeliveryPorts& delivery,
                    PreferredThreadFn preferred_thread);

  Roe<std::string> LocalRelayIdentity() const;
  /** The E2E direct thread call-control to `peer_identity` goes over (created when missing). */
  Roe<std::string> EnsureCallControlThread(const std::string& peer_identity);
  Roe<void> SendDirect(const std::string& peer_identity, CallControlType type, const std::string& detail_json,
                       const std::string& display);
  Roe<void> AppendOriginHistory(const std::string& thread_id, CallControlType type, const std::string& text,
                                const std::string& detail_json);
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
  ContactsStore& contacts_;
  IdentityStore& identity_;
  CallSessionStore& sessions_;
  IPskSessionStore& psk_store_;
  const CallDeliveryPorts& delivery_;
  PreferredThreadFn preferred_thread_;
  /**
   * AutoKey key_init from ensure_peer_session_key — attached on the next SendDirect to that peer
   * (the peer needs it to derive the session key the media key is wrapped under).
   */
  mutable std::mutex pending_key_init_mutex_;
  mutable std::unordered_map<std::string, std::string> pending_key_init_;
};

} // namespace pbr
