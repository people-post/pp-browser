#pragma once

#include "common/Error.h"
#include "common/Module.h"
#include "common/thread/IThreadTranscript.h"
#include "lib/calls/CallControlCodec.h"
#include "lib/calls/CallSummaryLogic.h"
#include "lib/calls/ICallSessionReader.h"
#include "common/directory/ILocalAccount.h"
#include "feature/calls/CallDeliveryPorts.h"

#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * Outbound call-control I/O: signaling to a peer on the control lane, fan-out to a call's
 * participants, the media key wrapped for a peer, and the call's summary in its thread. The call
 * session manager decides what to send; this sends it. Calls owner.
 */
class CallControlClient : public Module {
public:
  CallControlClient(IThreadTranscript& transcript, const ILocalAccount& account, const ICallSessionReader& sessions,
                    const CallDeliveryPorts& delivery);

  Roe<std::string> LocalRelayIdentity() const;
  /** Signaling to `peer_identity` on the control lane. */
  Roe<void> SendDirect(const std::string& peer_identity, CallControlType type, const std::string& detail_json,
                       const std::string& display);
  /**
   * The call's summary into `thread_id`'s chat stream (both sides, all their devices read it); kept
   * here only when it cannot be sent (an unverified private key, no route) or `local_only`.
   */
  Roe<void> SendSummary(const std::string& thread_id, const CallSummaryDetail& summary, bool local_only = false,
                        SummaryWrite write = SummaryWrite::Final);
  /**
   * The same signaling to each of `peers`, best-effort (V058 2): sealed per pair off the calls loop, in parallel;
   * a peer that cannot be sent to is logged and does not hold up the rest.
   */
  void SendToEach(const std::vector<std::string>& peers, CallControlType type, const std::string& detail_json,
                  const std::string& display);
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

  /**
   * The e2e_public session key with `peer_identity` (delivery's: opened when missing) — media keys are
   * wrapped under it. A key_init the session still carries rides the next SendDirect to that peer.
   */
  Roe<ByteVector> SessionKeyWith(const std::string& peer_identity) const;

private:
  IThreadTranscript& transcript_;
  const ILocalAccount& account_;
  const ICallSessionReader& sessions_;
  const CallDeliveryPorts& delivery_;
  /**
   * AutoKey key_init from ensure_peer_session_key — rides the next SendDirect to that peer (the
   * peer needs it to open the session the media key is wrapped under).
   */
  mutable std::mutex pending_key_init_mutex_;
  mutable std::unordered_map<std::string, std::string> pending_key_init_;
};

} // namespace pbr
