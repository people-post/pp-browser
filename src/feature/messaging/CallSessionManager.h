#pragma once

#include "base/crypto/IPskSessionStore.h"
#include "base/media/CallMediaEngine.h"
#include "base/messaging/CallControlCodec.h"
#include "base/messaging/CallSessionStore.h"
#include "base/messaging/IThreadStore.h"
#include "base/people/ContactsStore.h"
#include "base/people/IdentityStore.h"
#include "feature/messaging/CallMediaKeyStore.h"
#include "feature/messaging/P2pMessagingService.h"

#include "common/Module.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace pbr {

/**
 * Call session lifecycle + pairwise signaling + media bring-up (a2 / V014).
 * Invite-only, hostless, min-identity epoch coordinator (V002/V005/V012).
 */
class CallSessionManager : public Module {
public:
  using RingChangedFn = std::function<void()>;

  CallSessionManager(IThreadStore& store, ContactsStore& contacts, IdentityStore& identity,
                     CallSessionStore& sessions, CallMediaKeyStore& media_keys, P2pMessagingService& p2p,
                     IPskSessionStore& psk_store, CallMediaEngine& media);

  void SetOnRingChanged(RingChangedFn callback);

  /** Start a 1:1 (or multi-invite) call linked to origin_thread_id. Invites peer identities. */
  Roe<CallSession> StartCall(const std::string& origin_thread_id, CallMediaMode mode,
                             const std::vector<std::string>& invitee_identities);

  Roe<void> AcceptInvite(const std::string& call_id);
  Roe<void> DeclineInvite(const std::string& call_id);
  Roe<void> LeaveCall(const std::string& call_id);

  /** Mid-call guest invite (pairwise only; no group membership). */
  Roe<void> InviteParticipant(const std::string& call_id, const std::string& invitee_identity);

  Roe<std::vector<PendingCallInvite>> ListPendingInvites();
  Roe<std::optional<CallSession>> ActiveLocalCall() const;
  Roe<std::optional<PendingCallInvite>> TopPendingInvite();

  /** First non-local participant identity for a call (display / peer label). */
  Roe<std::optional<std::string>> PeerIdentityForCall(const std::string& call_id) const;

  /** Peer (non-local) video_enabled from roster; nullopt if no peer row. */
  Roe<std::optional<bool>> PeerVideoEnabledForCall(const std::string& call_id) const;

  /** Expire stale pending invites; notify UI if any changed. */
  void SweepExpiredInvites();

  /** Apply inbound pairwise call system control (after DM persist). */
  Roe<void> ApplyInboundControl(ThreadMessage& message, const std::string& sender_identity);

  CallMediaEngine& Media();

  /** Local mute / camera — updates participant row + roster fan-out (V019 content policy). */
  Roe<void> SetLocalAudioMuted(bool muted);
  Roe<void> SetLocalVideoEnabled(bool enabled);

  /** Pop last media start failure (for UI toast). Empty if none. */
  std::optional<std::string> TakeLastMediaError();

  /** Drop media callbacks before destroying this manager (avoids UAF on engine Stop). */
  void ClearMediaCallbacks();

private:
  Roe<std::string> LocalRelayIdentity() const;
  Roe<void> SendCallDirectMessage(const std::string& peer_identity, CallControlType type,
                                  const std::string& detail_json, const std::string& display);
  Roe<void> AppendOriginHistory(const std::string& thread_id, CallControlType type, const std::string& text,
                                const std::string& detail_json);
  Roe<void> FanOutToJoined(const std::string& call_id, CallControlType type, const std::string& detail_json,
                           const std::string& display, const std::string& skip_identity);
  Roe<void> MaybeRotateMediaKey(const std::string& call_id, const std::string& leaver_identity);
  Roe<void> EndCallLocal(CallSession& session, const std::optional<int64_t>& duration_ms);
  Roe<CallRosterDetail> BuildRosterDetail(const std::string& call_id) const;
  void NotifyRingChanged();

  Roe<ByteVector> ResolvePeerSessionKey(const std::string& peer_identity) const;
  Roe<void> SendMediaKeyToPeer(const std::string& call_id, const std::string& peer_identity,
                               uint32_t media_epoch, const std::string& media_key_id, const ByteVector& key_bytes);
  Roe<void> StartMediaAsOfferer(const std::string& call_id, const std::string& peer_identity);
  Roe<void> StartMediaAsAnswerer(const std::string& call_id, const std::string& peer_identity);
  void BindMediaCallbacks(const std::string& peer_identity);
  void StopMediaIfCall(const std::string& call_id);
  /** End any other Joined local session before accepting/starting a different call. */
  Roe<void> LeaveCallIfActiveExcept(const std::string& keep_call_id);

  IThreadStore& store_;
  ContactsStore& contacts_;
  IdentityStore& identity_;
  CallSessionStore& sessions_;
  CallMediaKeyStore& media_keys_;
  P2pMessagingService& p2p_;
  IPskSessionStore& psk_store_;
  CallMediaEngine& media_;
  RingChangedFn on_ring_changed_;
  std::string media_peer_identity_;
  std::optional<std::string> last_media_error_;
};

} // namespace pbr
