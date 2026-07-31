#include "feature/messaging/CallSessionManager.h"

#include "base/crypto/CryptoUtil.h"
#include "base/crypto/SessionKeyDeriver.h"
#include "base/messaging/CallSessionLogic.h"
#include "base/messaging/DirectChatTarget.h"
#include "base/messaging/SendRelayOptions.h"
#include "base/people/ContactJson.h"
#include "base/people/ContactTypes.h"
#include "base/platform/BrowserThread.h"
#include "common/Utilities.h"

#include <algorithm>
#include <nlohmann/json.hpp>

namespace pbr {
namespace {

/** How long invitees wait for CallSfuAttach / hop attach before leaving (no relay / migrate fail). */
constexpr int64_t kSfuAttachWaitMs = 20000;
/** 1:1 P2P: give up connecting chrome and show Retry after this. */
constexpr int64_t kP2pConnectTimeoutMs = 15000;

} // namespace

CallSessionManager::CallSessionManager(IThreadStore& store, ContactsStore& contacts, IdentityStore& identity,
                                       CallSessionStore& sessions, CallMediaKeyStore& media_keys,
                                       P2pMessagingService& p2p, IPskSessionStore& psk_store, CallMediaEngine& media)
    : store_(store), contacts_(contacts), identity_(identity), sessions_(sessions), media_keys_(media_keys),
      p2p_(p2p), psk_store_(psk_store), media_(media) {
  redirectLogger("CallSessionManager");
}

void CallSessionManager::SetMediaRelayDeps(MediaRelayDeps deps) {
  relay_deps_ = std::move(deps);
}

CallMediaEngine& CallSessionManager::Media() {
  return media_;
}

Roe<void> CallSessionManager::SetLocalAudioMuted(bool muted) {
  auto local = LocalRelayIdentity();
  if (!local) {
    return local.error();
  }
  const std::string call_id = media_.ActiveCallId();
  if (call_id.empty()) {
    return Error("No active call media");
  }
  media_.SetMuted(muted);
  auto participant = sessions_.FindParticipant(call_id, *local);
  if (participant && participant->has_value()) {
    (*participant)->media.audio_muted = muted;
    (void)sessions_.UpsertParticipant(**participant);
  }
  auto roster = BuildRosterDetail(call_id);
  if (roster) {
    auto roster_json = CallControlCodec::EncodeRoster(*roster);
    if (roster_json) {
      (void)FanOutToJoined(call_id, CallControlType::CallRoster, *roster_json, "Call roster", *local);
    }
  }
  NotifyRingChanged();
  return {};
}

Roe<void> CallSessionManager::SetLocalVideoEnabled(bool enabled) {
  auto local = LocalRelayIdentity();
  if (!local) {
    return local.error();
  }
  const std::string call_id = media_.ActiveCallId();
  if (call_id.empty()) {
    return Error("No active call media");
  }
  RefreshAdaptation(call_id);
  if (enabled) {
    if (auto cam = media_.SetCameraEnabled(true); !cam) {
      return cam.error();
    }
  } else {
    (void)media_.SetCameraEnabled(false);
  }
  auto participant = sessions_.FindParticipant(call_id, *local);
  if (participant && participant->has_value()) {
    (*participant)->media.video_enabled = enabled && media_.IsCameraEnabled();
    (void)sessions_.UpsertParticipant(**participant);
  }
  auto roster = BuildRosterDetail(call_id);
  if (roster) {
    auto roster_json = CallControlCodec::EncodeRoster(*roster);
    if (roster_json) {
      (void)FanOutToJoined(call_id, CallControlType::CallRoster, *roster_json, "Call roster", *local);
    }
  }
  NotifyRingChanged();
  return {};
}

std::optional<std::string> CallSessionManager::TakeLastMediaError() {
  auto out = std::move(last_media_error_);
  last_media_error_.reset();
  return out;
}

void CallSessionManager::SetOnRingChanged(RingChangedFn callback) {
  on_ring_changed_ = std::move(callback);
}

void CallSessionManager::NotifyRingChanged() {
  if (on_ring_changed_) {
    on_ring_changed_();
  }
}

Roe<std::string> CallSessionManager::LocalRelayIdentity() const {
  auto identity = identity_.Get();
  if (!identity || identity->relay_user_id.empty()) {
    return Error("Local relay identity unavailable");
  }
  return identity->relay_user_id;
}

Roe<void> CallSessionManager::SendCallDirectMessage(const std::string& peer_identity, const CallControlType type,
                                                    const std::string& detail_json, const std::string& display) {
  DirectChatTarget direct_target;
  direct_target.peer_identity_kind = ContactIdKindToString(ContactIdKind::RelayUser);
  direct_target.peer_identity_value = peer_identity;
  direct_target.channel = ThreadChannel::E2ePublic;

  std::string contact_id;
  std::string dm_title = peer_identity;
  if (auto contact = contacts_.FindByIdentity(peer_identity, ContactIdKind::RelayUser)) {
    if (*contact) {
      contact_id = (*contact)->id;
      dm_title = (*contact)->display_name.empty() ? (*contact)->server_nickname : (*contact)->display_name;
      if (dm_title.empty()) {
        dm_title = peer_identity;
      }
    }
  }

  auto thread = store_.FindOrCreateDirectThread(direct_target, contact_id, dm_title);
  if (!thread) {
    return thread.error();
  }

  SendRelayOptions opts;
  opts.content_type = ChatContentType::System;
  opts.payload_json =
      nlohmann::json({{"control_type", CallControlTypeToWire(type)}, {"detail", detail_json}}).dump();
  opts.generation = "system";
  opts.update_preview = false;
  auto sent = p2p_.SendUserMessage(thread->id, display, opts);
  if (!sent) {
    return sent.error();
  }
  return {};
}

Roe<void> CallSessionManager::AppendOriginHistory(const std::string& thread_id, const CallControlType type,
                                                  const std::string& text, const std::string& detail_json) {
  auto local = LocalRelayIdentity();
  if (!local) {
    return local.error();
  }
  auto message = CallControlCodec::BuildSystemMessage(thread_id, type, text, detail_json, *local);
  if (!message) {
    return message.error();
  }
  if (auto appended = store_.AppendMessage(*message); !appended) {
    return appended.error();
  }
  return {};
}

Roe<CallRosterDetail> CallSessionManager::BuildRosterDetail(const std::string& call_id) const {
  auto session = sessions_.LoadSession(call_id);
  if (!session || !session->has_value()) {
    return Error("Call session not found");
  }
  auto participants = sessions_.ListParticipants(call_id);
  if (!participants) {
    return participants.error();
  }
  CallRosterDetail detail;
  detail.call_id = call_id;
  detail.media_epoch = (*session)->media_epoch;
  for (const CallParticipant& row : *participants) {
    CallRosterEntry entry;
    entry.identity = row.identity;
    entry.state = row.state;
    entry.audio_muted = row.media.audio_muted;
    entry.video_enabled = row.media.video_enabled;
    detail.participants.push_back(std::move(entry));
  }
  return detail;
}

Roe<void> CallSessionManager::FanOutToJoined(const std::string& call_id, const CallControlType type,
                                             const std::string& detail_json, const std::string& display,
                                             const std::string& skip_identity) {
  auto participants = sessions_.ListParticipants(call_id);
  if (!participants) {
    return participants.error();
  }
  for (const CallParticipant& row : *participants) {
    if (row.identity == skip_identity || row.state != CallParticipantState::Joined) {
      continue;
    }
    if (auto sent = SendCallDirectMessage(row.identity, type, detail_json, display); !sent) {
      return sent.error();
    }
  }
  return {};
}

Roe<ByteVector> CallSessionManager::ResolvePeerSessionKey(const std::string& peer_identity) const {
  ChatTargetKey target_key;
  target_key.peer_identity_kind = ContactIdKindToString(ContactIdKind::RelayUser);
  target_key.peer_identity_value = peer_identity;
  target_key.channel = CryptoChannel::E2ePublic;

  auto record = psk_store_.Load(target_key);
  if (!record) {
    return record.error();
  }
  if (!record->has_value()) {
    return Error("No PSK session for peer");
  }
  const uint32_t active_epoch = (*record)->session_epoch;
  auto master_psk_b64 = psk_store_.ResolveMasterPskForEpoch(target_key, active_epoch);
  if (!master_psk_b64) {
    return master_psk_b64.error();
  }
  if (!master_psk_b64->has_value()) {
    return Error("No PSK for active session epoch");
  }
  auto master_psk = Base64Decode(**master_psk_b64);
  if (!master_psk) {
    return master_psk.error();
  }
  return SessionKeyDeriver::Derive(*master_psk, CryptoChannel::E2ePublic, active_epoch);
}

Roe<void> CallSessionManager::SendMediaKeyToPeer(const std::string& call_id, const std::string& peer_identity,
                                                 const uint32_t media_epoch, const std::string& media_key_id,
                                                 const ByteVector& key_bytes) {
  auto session_key = ResolvePeerSessionKey(peer_identity);
  if (!session_key) {
    return session_key.error();
  }
  auto wrapped = CallMediaKeyStore::WrapKeyB64(*session_key, key_bytes, call_id, media_epoch, media_key_id);
  if (!wrapped) {
    return wrapped.error();
  }
  CallMediaKeyDetail key_detail;
  key_detail.call_id = call_id;
  key_detail.media_epoch = media_epoch;
  key_detail.media_key_id = media_key_id;
  key_detail.wrapped_key_b64 = *wrapped;
  auto key_json = CallControlCodec::EncodeMediaKey(key_detail);
  if (!key_json) {
    return key_json.error();
  }
  return SendCallDirectMessage(peer_identity, CallControlType::CallMediaKey, *key_json, "Call media key");
}

void CallSessionManager::BindMediaCallbacks(const std::string& peer_identity) {
  media_peer_identity_ = peer_identity;

  media_.SetOnLocalDescription([this](const CallMediaEngine::LocalDescription& local) {
    // Prefer media_call_id_ — ActiveCallId locks and may run under Start()'s mutex.
    const std::string call_id = !media_call_id_.empty() ? media_call_id_ : media_.ActiveCallId();
    if (call_id.empty() || media_peer_identity_.empty()) {
      return;
    }
    // Hop off the PC/Start stack (media mutex may be held during answerer Flush).
    const std::string peer = media_peer_identity_;
    BrowserThread::PostTask(BrowserThreadId::UI, [this, call_id, peer, local]() {
      if (media_call_id_ != call_id && media_.ActiveCallId() != call_id) {
        return;
      }
      CallSdpDetail detail;
      detail.call_id = call_id;
      if (auto local_identity = LocalRelayIdentity()) {
        detail.identity = *local_identity;
      }
      detail.sdp_type = local.type;
      detail.sdp = local.sdp;
      auto encoded = CallControlCodec::EncodeSdp(detail);
      if (!encoded) {
        return;
      }
      (void)SendCallDirectMessage(peer, CallControlType::CallSdp, *encoded, "Call signaling");
    });
  });

  media_.SetOnIceCandidate([this](const CallMediaEngine::IceCandidate& ice) {
    const std::string call_id = !media_call_id_.empty() ? media_call_id_ : media_.ActiveCallId();
    if (call_id.empty() || media_peer_identity_.empty()) {
      return;
    }
    const std::string peer = media_peer_identity_;
    BrowserThread::PostTask(BrowserThreadId::UI, [this, call_id, peer, ice]() {
      if (media_call_id_ != call_id && media_.ActiveCallId() != call_id) {
        return;
      }
      CallIceDetail detail;
      detail.call_id = call_id;
      if (auto local_identity = LocalRelayIdentity()) {
        detail.identity = *local_identity;
      }
      detail.candidate = ice.candidate;
      detail.mid = ice.mid;
      auto encoded = CallControlCodec::EncodeIce(detail);
      if (!encoded) {
        return;
      }
      (void)SendCallDirectMessage(peer, CallControlType::CallIce, *encoded, "Call signaling");
    });
  });

  media_.SetOnStateChanged([this](const std::string& state) {
    // Must not call locking CallMediaEngine APIs or MaybeSoftMigrateToSfu here when
    // SetState still races with Start/Stop — use the call id we bound at media start.
    const std::string call_id = media_call_id_;
    // Only `failed` is an ICE failure. `closed` is normal teardown (Stop/Leave) and must
    // not start SFU attach-wait — that was mis-firing 1:1 P2P into "group needs media_relay".
    if (state == "connected") {
      ClearP2pConnectFailed();
      if (media_.IsSfuMode()) {
        awaiting_sfu_recovery_ = false;
      }
      NotifyRingChanged();
      return;
    }
    if (state == "failed" && !media_.IsSfuMode() && !call_id.empty()) {
      auto joined = sessions_.CountJoined(call_id);
      // Group (N≥3) must recover onto SFU. Do not auto-SFU plain 1:1 — that path hung on
      // attach-wait and toasted the group-call error when no hop exists.
      if (joined && *joined >= 3) {
        awaiting_sfu_recovery_ = true;
        // Quote/dial can take seconds — never run on the media/PC callback stack.
        BrowserThread::PostTask(BrowserThreadId::UI, [this, call_id]() {
          if (sfu_attached_ && media_.IsSfuMode()) {
            awaiting_sfu_recovery_ = false;
            NotifyRingChanged();
            return;
          }
          auto migrated = MaybeSoftMigrateToSfu(call_id);
          if (sfu_attached_) {
            awaiting_sfu_recovery_ = false;
            ClearSfuAttachWait();
            NotifyRingChanged();
            return;
          }
          if (!migrated) {
            awaiting_sfu_recovery_ = false;
            last_media_error_ =
                migrated.error().message.empty()
                    ? "No media relay available — group call needs a media_relay hop"
                    : migrated.error().message;
            log().warning << "ICE-fail SFU recovery failed: " << *last_media_error_;
            (void)LeaveCall(call_id);
            NotifyRingChanged();
            return;
          }
          // Non-coordinator: SoftMigrate is a no-op until CallSfuAttach arrives.
          BeginSfuAttachWait(call_id);
          NotifyRingChanged();
        });
        return;
      }
      // 1:1 — keep session; UI shows Couldn't connect + Retry (not auto-leave).
      MarkP2pConnectFailed("ice_failed");
      NotifyRingChanged();
      return;
    }
    NotifyRingChanged();
  });
}

void CallSessionManager::ClearMediaCallbacks() {
  media_.SetOnLocalDescription({});
  media_.SetOnIceCandidate({});
  media_.SetOnStateChanged({});
  media_peer_identity_.clear();
  media_call_id_.clear();
}

Roe<void> CallSessionManager::StartMediaAsOfferer(const std::string& call_id, const std::string& peer_identity) {
  media_attempted_calls_.insert(call_id);
  media_call_id_ = call_id;
  BindMediaCallbacks(peer_identity);
  return media_.Start(call_id, CallMediaEngine::Role::Offerer);
}

Roe<void> CallSessionManager::StartMediaAsAnswerer(const std::string& call_id, const std::string& peer_identity) {
  media_attempted_calls_.insert(call_id);
  media_call_id_ = call_id;
  BindMediaCallbacks(peer_identity);
  return media_.Start(call_id, CallMediaEngine::Role::Answerer);
}

void CallSessionManager::ScheduleStartMediaAsOfferer(const std::string& call_id,
                                                     const std::string& peer_identity) {
  // Mark before PostTask so RefreshPendingRing does not treat the session as a boot orphan.
  media_attempted_calls_.insert(call_id);
  BrowserThread::PostTask(BrowserThreadId::UI, [this, call_id, peer_identity]() {
    auto session = sessions_.LoadSession(call_id);
    if (!session || !session->has_value() || (*session)->state == CallSessionState::Ended) {
      return;
    }
    if (media_.IsActive() && media_.ActiveCallId() == call_id) {
      return;
    }
    if (auto started = StartMediaAsOfferer(call_id, peer_identity); !started) {
      log().warning << "StartMediaAsOfferer failed: " << started.error().message;
      last_media_error_ = started.error().message;
      NotifyRingChanged();
      return;
    }
    // Fast peers (Linux) often deliver the first offer before Mac answerer Start(). If that
    // buffered offer was dropped, a second copy on the next UI turn recovers the call.
    // Answerer ignores duplicates once applied. Android→Mac rarely needs this (slower offerer).
    BrowserThread::PostTask(BrowserThreadId::UI, [this, call_id, peer_identity]() {
      if (!media_.IsActive() || media_.ActiveCallId() != call_id || media_.IsConnected() ||
          media_.IsSfuMode()) {
        return;
      }
      auto local = media_.CurrentLocalDescription();
      if (!local || local->sdp.empty()) {
        return;
      }
      CallSdpDetail detail;
      detail.call_id = call_id;
      if (auto local_identity = LocalRelayIdentity()) {
        detail.identity = *local_identity;
      }
      detail.sdp_type = local->type;
      detail.sdp = local->sdp;
      auto encoded = CallControlCodec::EncodeSdp(detail);
      if (!encoded) {
        return;
      }
      log().info << "Re-sending local offer for answerer race call_id=" << call_id;
      (void)SendCallDirectMessage(peer_identity, CallControlType::CallSdp, *encoded, "Call signaling");
    });
    NotifyRingChanged();
  });
}

void CallSessionManager::ScheduleStartMediaAsAnswerer(const std::string& call_id,
                                                      const std::string& peer_identity) {
  media_attempted_calls_.insert(call_id);
  BrowserThread::PostTask(BrowserThreadId::UI, [this, call_id, peer_identity]() {
    auto session = sessions_.LoadSession(call_id);
    if (!session || !session->has_value() || (*session)->state == CallSessionState::Ended) {
      return;
    }
    // Always call Start — if already active it still flushes a late-buffered offer.
    if (auto started = StartMediaAsAnswerer(call_id, peer_identity); !started) {
      log().warning << "StartMediaAsAnswerer failed: " << started.error().message;
      last_media_error_ = started.error().message;
    }
    NotifyRingChanged();
  });
}

void CallSessionManager::StopMediaIfCall(const std::string& call_id) {
  if (media_.IsActive() && media_.ActiveCallId() == call_id) {
    media_.Stop();
  }
  if (sfu_attached_ && relay_deps_.relay) {
    relay_deps_.relay->Detach();
  }
  sfu_attached_ = false;
  awaiting_sfu_recovery_ = false;
  ClearP2pConnectFailed();
  local_publisher_stream_id_ = 0;
  if (sfu_attach_wait_call_id_ == call_id) {
    ClearSfuAttachWait();
  }
  ClearMediaCallbacks();
  media_attempted_calls_.erase(call_id);
}

Roe<void> CallSessionManager::LeaveCallIfActiveExcept(const std::string& keep_call_id) {
  // Drain any conflicting Joined sessions (normally at most one).
  for (int i = 0; i < 4; ++i) {
    auto active = ActiveLocalCall();
    if (!active) {
      return active.error();
    }
    if (!active->has_value() || (*active)->call_id == keep_call_id) {
      return {};
    }
    if (auto left = LeaveCall((*active)->call_id); !left) {
      log().warning << "LeaveCallIfActiveExcept failed for " << (*active)->call_id << ": "
                    << left.error().message;
      return left.error();
    }
  }
  return {};
}

Roe<CallSession> CallSessionManager::StartCall(const std::string& origin_thread_id, const CallMediaMode mode,
                                               const std::vector<std::string>& invitee_identities) {
  if (invitee_identities.empty()) {
    return Error("At least one invitee required");
  }
  auto local = LocalRelayIdentity();
  if (!local) {
    return local.error();
  }
  auto pending = TopPendingInvite();
  if (!pending) {
    return pending.error();
  }
  if (pending->has_value()) {
    return Error("Decline the incoming call first");
  }
  auto thread = store_.GetThread(origin_thread_id);
  if (!thread || !*thread) {
    return Error("Origin thread not found");
  }
  if ((*thread)->kind != ThreadKind::Direct && (*thread)->kind != ThreadKind::Group) {
    return Error("Calls require a direct or group thread");
  }

  auto key = media_keys_.GenerateEpochKey();
  if (!key) {
    return key.error();
  }
  const std::string call_id = GenerateCallId();
  auto media_key_id = media_keys_.PutEpochKey(call_id, 1, *key);
  if (!media_key_id) {
    return media_key_id.error();
  }

  const int64_t now = util::NowUnixMs();
  CallSession session;
  session.call_id = call_id;
  session.origin_thread_id = origin_thread_id;
  if ((*thread)->kind == ThreadKind::Group && (*thread)->group_id) {
    session.origin_group_id = *(*thread)->group_id;
  }
  session.media_mode = mode;
  session.state = CallSessionState::Ringing;
  session.created_at = now;
  session.media_epoch = 1;
  session.media_key_id = *media_key_id;
  if (auto saved = sessions_.UpsertSession(session); !saved) {
    return saved.error();
  }

  CallParticipant self;
  self.call_id = call_id;
  self.identity = *local;
  self.state = CallParticipantState::Joined;
  self.media.video_enabled = false;
  self.joined_at = now;
  if (auto saved = sessions_.UpsertParticipant(self); !saved) {
    return saved.error();
  }

  CallStartedDetail started;
  started.call_id = call_id;
  started.media_mode = mode;
  auto started_detail = CallControlCodec::EncodeStarted(started);
  if (!started_detail) {
    return started_detail.error();
  }
  const std::string started_text = mode == CallMediaMode::Video ? "Video call started" : "Voice call started";
  if (auto hist = AppendOriginHistory(origin_thread_id, CallControlType::CallStarted, started_text, *started_detail);
      !hist) {
    return hist.error();
  }

  for (const std::string& invitee : invitee_identities) {
    if (invitee.empty() || invitee == *local) {
      continue;
    }
    if (auto invited = InviteParticipant(call_id, invitee); !invited) {
      return invited.error();
    }
  }

  NotifyRingChanged();
  return session;
}

Roe<void> CallSessionManager::InviteParticipant(const std::string& call_id, const std::string& invitee_identity) {
  auto local = LocalRelayIdentity();
  if (!local) {
    return local.error();
  }
  auto session = sessions_.LoadSession(call_id);
  if (!session || !session->has_value()) {
    return Error("Call session not found");
  }
  if ((*session)->state == CallSessionState::Ended) {
    return Error("Call has ended");
  }
  auto joined = sessions_.CountJoined(call_id);
  if (!joined) {
    return joined.error();
  }
  if (!CallSessionLogic::CanAcceptJoin(*joined)) {
    return Error("Call is full");
  }
  // N≥3 requires media_relay soft-migrate (V021). Refuse mid-call guest invites when no hop
  // exists — otherwise Mac/Linux stay on 1:1 P2P while the invitee hangs on Connecting….
  const bool already_on_sfu =
      (sfu_attached_ && media_.IsSfuMode() && media_.ActiveCallId() == call_id) ||
      ((*session)->sfu_hint && !(*session)->sfu_hint->empty());
  if (*joined >= 2 && !already_on_sfu && !HasMediaRelayHopCandidates()) {
    return Error("Adding a guest needs a media relay hop (enable Media relay on a Node/seed)");
  }

  const int64_t now = util::NowUnixMs();
  CallParticipant participant;
  participant.call_id = call_id;
  participant.identity = invitee_identity;
  participant.state = CallParticipantState::Ringing;
  if (auto saved = sessions_.UpsertParticipant(participant); !saved) {
    return saved.error();
  }

  PendingCallInvite pending;
  pending.call_id = call_id;
  pending.inviter_identity = *local;
  pending.invitee_identity = invitee_identity;
  pending.media_mode = (*session)->media_mode;
  pending.origin_thread_id = (*session)->origin_thread_id;
  pending.origin_group_id = (*session)->origin_group_id;
  pending.sfu_hint = (*session)->sfu_hint;
  pending.expires_at = now + kDefaultCallInviteTtlMs;
  pending.created_at = now;
  pending.status = "pending";
  if (auto saved = sessions_.UpsertPendingInvite(pending); !saved) {
    return saved.error();
  }

  CallInviteDetail invite;
  invite.call_id = call_id;
  invite.inviter_identity = *local;
  invite.invitee_identity = invitee_identity;
  invite.media_mode = (*session)->media_mode;
  invite.origin_thread_id = (*session)->origin_thread_id;
  invite.origin_group_id = (*session)->origin_group_id;
  invite.sfu_hint = (*session)->sfu_hint;
  invite.expires_at = pending.expires_at;
  auto detail = CallControlCodec::EncodeInvite(invite);
  if (!detail) {
    return detail.error();
  }
  const std::string display =
      (*session)->media_mode == CallMediaMode::Video ? "Incoming video call" : "Incoming voice call";
  return SendCallDirectMessage(invitee_identity, CallControlType::CallInvite, *detail, display);
}

Roe<void> CallSessionManager::AcceptInvite(const std::string& call_id) {
  auto local = LocalRelayIdentity();
  if (!local) {
    return local.error();
  }
  SweepExpiredInvites();
  if (auto cleared = LeaveCallIfActiveExcept(call_id); !cleared) {
    return cleared.error();
  }
  auto pending = sessions_.LoadPendingInvite(call_id, *local);
  if (!pending || !pending->has_value() || (*pending)->status != "pending") {
    return Error("Pending call invite not found");
  }
  if (CallSessionLogic::IsInviteExpired(**pending, util::NowUnixMs())) {
    (void)sessions_.UpdateInviteStatus(call_id, *local, "expired");
    NotifyRingChanged();
    return Error("Call invite expired");
  }

  auto session = sessions_.LoadSession(call_id);
  CallSession row;
  if (session && session->has_value()) {
    row = **session;
  } else {
    row.call_id = call_id;
    row.origin_thread_id = (*pending)->origin_thread_id;
    row.origin_group_id = (*pending)->origin_group_id;
    row.media_mode = (*pending)->media_mode;
    row.state = CallSessionState::Ringing;
    row.created_at = (*pending)->created_at;
    row.media_epoch = 1;
    row.sfu_hint = (*pending)->sfu_hint;
  }

  auto joined = sessions_.CountJoined(call_id);
  const size_t joined_count = joined ? *joined : 0;
  if (!CallSessionLogic::CanAcceptJoin(joined_count)) {
    return Error("Call is full");
  }

  const int64_t now = util::NowUnixMs();
  row.state = CallSessionLogic::TransitionOnRemoteJoined(row.state);
  if (auto saved = sessions_.UpsertSession(row); !saved) {
    return saved.error();
  }

  CallParticipant self;
  self.call_id = call_id;
  self.identity = *local;
  self.state = CallParticipantState::Joined;
  self.media.video_enabled = false;
  self.joined_at = now;
  if (auto saved = sessions_.UpsertParticipant(self); !saved) {
    return saved.error();
  }
  (void)sessions_.UpdateInviteStatus(call_id, *local, "accepted");

  CallAcceptDetail accept;
  accept.call_id = call_id;
  accept.identity = *local;
  accept.video_enabled = false;
  auto detail = CallControlCodec::EncodeAccept(accept);
  if (!detail) {
    return detail.error();
  }
  if (auto sent = SendCallDirectMessage((*pending)->inviter_identity, CallControlType::CallAccept, *detail,
                                        "Call accepted");
      !sent) {
    return sent.error();
  }

  auto joined_after = sessions_.CountJoined(call_id);
  const size_t n_joined = joined_after ? *joined_after : 0;
  // sfu_hint is only meaningful for group (N≥3) / already-on-SFU calls. A leftover hint on a
  // 1:1 invite must not divert P2P into AttachLocalToSfu → "group needs media_relay".
  if (n_joined >= 3 && row.sfu_hint && !row.sfu_hint->empty()) {
    CallSfuAttachDetail attach;
    attach.call_id = call_id;
    attach.hop_peer_id = *row.sfu_hint;
    attach.publisher_stream_id = PublisherStreamIdForLocal();
    media_attempted_calls_.insert(call_id);
    BeginSfuAttachWait(call_id);
    BrowserThread::PostTask(BrowserThreadId::UI, [this, call_id, attach]() {
      if (auto ok = AttachLocalToSfu(call_id, attach); !ok) {
        log().warning << "AttachLocalToSfu (invite hint) failed: " << ok.error().message;
        last_media_error_ = ok.error().message;
        ClearSfuAttachWait();
        (void)LeaveCall(call_id);
      }
      NotifyRingChanged();
    });
  } else if (CallMediaTopology::ShouldUseMediaRelay(n_joined)) {
    media_attempted_calls_.insert(call_id);
    BeginSfuAttachWait(call_id);
    BrowserThread::PostTask(BrowserThreadId::UI, [this, call_id]() {
      if (auto mig = MaybeSoftMigrateToSfu(call_id); !mig) {
        log().warning << "MaybeSoftMigrateToSfu failed: " << mig.error().message;
        last_media_error_ = mig.error().message;
        ClearSfuAttachWait();
        (void)LeaveCall(call_id);
      } else if (!sfu_attached_) {
        // Non-coordinator: wait for CallSfuAttach (PollPendingSfuAttach times out).
      } else {
        ClearSfuAttachWait();
      }
      NotifyRingChanged();
    });
  } else {
    // 1:1 P2P — drop any stale SFU wait/hint so PollPendingSfuAttach cannot end the call.
    ClearSfuAttachWait();
    awaiting_sfu_recovery_ = false;
    if (row.sfu_hint && !row.sfu_hint->empty()) {
      row.sfu_hint.reset();
      (void)sessions_.UpsertSession(row);
    }
    // Must not Start media inside the Accept click / Rml callback — SDL audio + PC setup
    // can stall the UI so the ring dialog never dismisses.
    ScheduleStartMediaAsAnswerer(call_id, (*pending)->inviter_identity);
  }

  // Best-effort roster to inviter; full fan-out when we are coordinator later.
  auto roster = BuildRosterDetail(call_id);
  if (roster) {
    auto roster_json = CallControlCodec::EncodeRoster(*roster);
    if (roster_json) {
      (void)SendCallDirectMessage((*pending)->inviter_identity, CallControlType::CallRoster, *roster_json,
                                  "Call roster");
    }
  }

  NotifyRingChanged();
  return {};
}

Roe<void> CallSessionManager::DeclineInvite(const std::string& call_id) {
  auto local = LocalRelayIdentity();
  if (!local) {
    return local.error();
  }
  auto pending = sessions_.LoadPendingInvite(call_id, *local);
  if (!pending || !pending->has_value() || (*pending)->status != "pending") {
    return Error("Pending call invite not found");
  }
  (void)sessions_.UpdateInviteStatus(call_id, *local, "declined");

  CallParticipant participant;
  participant.call_id = call_id;
  participant.identity = *local;
  participant.state = CallParticipantState::Declined;
  (void)sessions_.UpsertParticipant(participant);

  CallDeclineDetail decline;
  decline.call_id = call_id;
  decline.identity = *local;
  auto detail = CallControlCodec::EncodeDecline(decline);
  if (!detail) {
    return detail.error();
  }
  if (auto sent = SendCallDirectMessage((*pending)->inviter_identity, CallControlType::CallDecline, *detail,
                                        "Call declined");
      !sent) {
    return sent.error();
  }
  NotifyRingChanged();
  return {};
}

Roe<void> CallSessionManager::MaybeRotateMediaKey(const std::string& call_id, const std::string& leaver_identity) {
  auto local = LocalRelayIdentity();
  if (!local) {
    return local.error();
  }
  auto participants = sessions_.ListParticipants(call_id);
  if (!participants) {
    return participants.error();
  }
  std::vector<std::string> remaining;
  for (const CallParticipant& row : *participants) {
    if (row.state == CallParticipantState::Joined && row.identity != leaver_identity) {
      remaining.push_back(row.identity);
    }
  }
  if (remaining.empty()) {
    return {};
  }
  auto coordinator = CallSessionLogic::SelectEpochCoordinator(remaining);
  if (!coordinator || *coordinator != *local) {
    return {};
  }

  auto session = sessions_.LoadSession(call_id);
  if (!session || !session->has_value()) {
    return Error("Call session not found");
  }
  auto key = media_keys_.GenerateEpochKey();
  if (!key) {
    return key.error();
  }
  const uint32_t new_epoch = (*session)->media_epoch + 1;
  auto media_key_id = media_keys_.PutEpochKey(call_id, new_epoch, *key);
  if (!media_key_id) {
    return media_key_id.error();
  }
  (*session)->media_epoch = new_epoch;
  (*session)->media_key_id = *media_key_id;
  if (auto saved = sessions_.UpsertSession(**session); !saved) {
    return saved.error();
  }

  auto roster = BuildRosterDetail(call_id);
  if (!roster) {
    return roster.error();
  }
  roster->media_epoch = new_epoch;
  auto roster_json = CallControlCodec::EncodeRoster(*roster);
  if (!roster_json) {
    return roster_json.error();
  }
  for (const std::string& peer : remaining) {
    if (peer == *local) {
      continue;
    }
    (void)SendMediaKeyToPeer(call_id, peer, new_epoch, *media_key_id, *key);
    (void)SendCallDirectMessage(peer, CallControlType::CallRoster, *roster_json, "Call roster");
  }
  return {};
}

Roe<void> CallSessionManager::EndCallLocal(CallSession& session, const std::optional<int64_t>& duration_ms) {
  StopMediaIfCall(session.call_id);
  session.state = CallSessionState::Ended;
  session.ended_at = util::NowUnixMs();
  if (auto saved = sessions_.UpsertSession(session); !saved) {
    return saved.error();
  }
  if (session.origin_thread_id) {
    CallEndedDetail ended;
    ended.call_id = session.call_id;
    ended.duration_ms = duration_ms;
    auto detail = CallControlCodec::EncodeEnded(ended);
    if (detail) {
      (void)AppendOriginHistory(*session.origin_thread_id, CallControlType::CallEnded, "Call ended", *detail);
    }
  }
  NotifyRingChanged();
  return {};
}

Roe<void> CallSessionManager::LeaveCall(const std::string& call_id) {
  auto local = LocalRelayIdentity();
  if (!local) {
    return local.error();
  }
  auto session = sessions_.LoadSession(call_id);
  if (!session || !session->has_value()) {
    return Error("Call session not found");
  }
  if ((*session)->state == CallSessionState::Ended) {
    return {};
  }
  StopMediaIfCall(call_id);

  const int64_t now = util::NowUnixMs();
  CallParticipant self;
  self.call_id = call_id;
  self.identity = *local;
  self.state = CallParticipantState::Left;
  self.left_at = now;
  if (auto saved = sessions_.UpsertParticipant(self); !saved) {
    return saved.error();
  }

  CallLeaveDetail leave;
  leave.call_id = call_id;
  leave.identity = *local;
  auto detail = CallControlCodec::EncodeLeave(leave);
  if (!detail) {
    return detail.error();
  }
  (void)FanOutToJoined(call_id, CallControlType::CallLeave, *detail, "Left the call", *local);

  auto joined = sessions_.CountJoined(call_id);
  const size_t remaining = joined ? *joined : 0;
  const CallSessionState next = CallSessionLogic::TransitionOnLeave((*session)->state, remaining);
  if (next == CallSessionState::Ended) {
    std::optional<int64_t> duration;
    if ((*session)->created_at > 0) {
      duration = now - (*session)->created_at;
    }
    CallEndedDetail ended;
    ended.call_id = call_id;
    ended.duration_ms = duration;
    auto ended_json = CallControlCodec::EncodeEnded(ended);
    if (ended_json) {
      (void)FanOutToJoined(call_id, CallControlType::CallEnded, *ended_json, "Call ended", *local);
    }
    return EndCallLocal(**session, duration);
  }

  (*session)->state = next;
  if (auto saved = sessions_.UpsertSession(**session); !saved) {
    return saved.error();
  }
  (void)MaybeRotateMediaKey(call_id, *local);
  NotifyRingChanged();
  return {};
}

Roe<std::vector<PendingCallInvite>> CallSessionManager::ListPendingInvites() {
  SweepExpiredInvites();
  auto local = LocalRelayIdentity();
  if (!local) {
    return local.error();
  }
  return sessions_.ListPendingInvitesForInvitee(*local);
}

Roe<std::optional<CallSession>> CallSessionManager::ActiveLocalCall() const {
  auto local = LocalRelayIdentity();
  if (!local) {
    return local.error();
  }
  auto active = sessions_.ListActiveSessions();
  if (!active) {
    return active.error();
  }
  for (const CallSession& session : *active) {
    auto participant = sessions_.FindParticipant(session.call_id, *local);
    if (participant && participant->has_value() && (*participant)->state == CallParticipantState::Joined) {
      return std::optional<CallSession>{session};
    }
  }
  return std::optional<CallSession>{};
}

Roe<std::optional<PendingCallInvite>> CallSessionManager::TopPendingInvite() {
  auto pending = ListPendingInvites();
  if (!pending) {
    return pending.error();
  }
  if (pending->empty()) {
    return std::optional<PendingCallInvite>{};
  }
  return std::optional<PendingCallInvite>{pending->front()};
}

Roe<std::optional<std::string>> CallSessionManager::PeerIdentityForCall(const std::string& call_id) const {
  auto local = LocalRelayIdentity();
  if (!local) {
    return local.error();
  }
  auto participants = sessions_.ListParticipants(call_id);
  if (!participants) {
    return participants.error();
  }
  for (const CallParticipant& row : *participants) {
    if (row.identity != *local && !row.identity.empty()) {
      return std::optional<std::string>{row.identity};
    }
  }
  return std::optional<std::string>{};
}

Roe<std::optional<bool>> CallSessionManager::PeerVideoEnabledForCall(const std::string& call_id) const {
  auto local = LocalRelayIdentity();
  if (!local) {
    return local.error();
  }
  auto participants = sessions_.ListParticipants(call_id);
  if (!participants) {
    return participants.error();
  }
  for (const CallParticipant& row : *participants) {
    if (row.identity != *local && !row.identity.empty()) {
      return std::optional<bool>{row.media.video_enabled};
    }
  }
  return std::optional<bool>{};
}

Roe<std::vector<CallParticipant>> CallSessionManager::ListJoinedParticipants(const std::string& call_id) const {
  auto participants = sessions_.ListParticipants(call_id);
  if (!participants) {
    return participants.error();
  }
  std::vector<CallParticipant> joined;
  joined.reserve(participants->size());
  for (const CallParticipant& row : *participants) {
    if (row.state == CallParticipantState::Joined) {
      joined.push_back(row);
    }
  }
  return joined;
}

bool CallSessionManager::IsAwaitingSfuRecovery() const { return awaiting_sfu_recovery_; }

bool CallSessionManager::IsP2pConnectFailed() const { return p2p_connect_failed_; }

bool CallSessionManager::P2pConnectMissingMic() const { return p2p_connect_missing_mic_; }

void CallSessionManager::ClearP2pConnectFailed() {
  p2p_connect_failed_ = false;
  p2p_connect_missing_mic_ = false;
}

void CallSessionManager::MarkP2pConnectFailed(const std::string& /*reason*/) {
  if (p2p_connect_failed_ || media_.IsSfuMode() || awaiting_sfu_recovery_) {
    return;
  }
  p2p_connect_failed_ = true;
  p2p_connect_missing_mic_ = media_.IsActive() && !media_.HasLocalCapture();
  // Peer Retry sends a new offer — accept rebuild even while still "connecting".
  media_.ArmOfferRestart();
  log().info << "P2P connect failed call_id=" << media_call_id_
             << " missing_mic=" << (p2p_connect_missing_mic_ ? "1" : "0");
}

void CallSessionManager::PollP2pConnectHealth() {
  if (p2p_connect_failed_ || media_.IsSfuMode() || awaiting_sfu_recovery_ || !media_.IsActive()) {
    return;
  }
  if (media_.IsConnected()) {
    ClearP2pConnectFailed();
    return;
  }
  const std::string call_id = media_.ActiveCallId();
  if (call_id.empty()) {
    return;
  }
  auto joined = sessions_.CountJoined(call_id);
  if (joined && *joined >= 3) {
    return; // group uses SFU attach-wait / ICE→SFU
  }
  if (media_.ConnectionState() == "failed") {
    MarkP2pConnectFailed("ice_failed");
    NotifyRingChanged();
    return;
  }
  const int64_t started = media_.StartedAtMs();
  if (started <= 0) {
    return;
  }
  if (util::NowUnixMs() - started < kP2pConnectTimeoutMs) {
    return;
  }
  MarkP2pConnectFailed("timeout");
  NotifyRingChanged();
}

Roe<void> CallSessionManager::RetryP2pMedia(const std::string& call_id) {
  if (call_id.empty()) {
    return Error("call_id required");
  }
  auto session = sessions_.LoadSession(call_id);
  if (!session || !session->has_value() || (*session)->state == CallSessionState::Ended) {
    return Error("Call session not found");
  }
  auto peer = PeerIdentityForCall(call_id);
  if (!peer || !peer->has_value() || (**peer).empty()) {
    return Error("No peer for call retry");
  }
  // Keep session + media_attempted; only rebuild the PeerConnection as offerer.
  ClearP2pConnectFailed();
  if (media_.IsActive() && media_.ActiveCallId() == call_id) {
    media_.Stop();
  }
  media_attempted_calls_.insert(call_id);
  media_call_id_ = call_id;
  BindMediaCallbacks(**peer);
  log().info << "Retrying P2P media as offerer call_id=" << call_id;
  return media_.Start(call_id, CallMediaEngine::Role::Offerer);
}

std::vector<MeshHopCandidate> CallSessionManager::RankedMediaHopCandidates() const {
  std::vector<Contact> contacts;
  if (auto listed = contacts_.List()) {
    contacts = std::move(*listed);
  }
  auto candidates = CollectContactHopCandidates(contacts);
  auto seeds = CollectSeedHopCandidates(relay_deps_.bootstrap_peers);
  candidates.insert(candidates.end(), seeds.begin(), seeds.end());
  return RankMediaHops(std::move(candidates));
}

bool CallSessionManager::HasMediaRelayHopCandidates() const {
  if (!relay_deps_.relay || !relay_deps_.sessions) {
    return false;
  }
  return !RankedMediaHopCandidates().empty();
}

void CallSessionManager::BeginSfuAttachWait(const std::string& call_id) {
  sfu_attach_wait_call_id_ = call_id;
  sfu_attach_wait_deadline_ms_ = util::NowUnixMs() + kSfuAttachWaitMs;
}

void CallSessionManager::ClearSfuAttachWait() {
  sfu_attach_wait_call_id_.clear();
  sfu_attach_wait_deadline_ms_ = 0;
}

void CallSessionManager::PollPendingSfuAttach() {
  if (sfu_attach_wait_call_id_.empty()) {
    return;
  }
  const std::string call_id = sfu_attach_wait_call_id_;
  if (sfu_attached_ && media_.IsSfuMode() && media_.ActiveCallId() == call_id) {
    ClearSfuAttachWait();
    awaiting_sfu_recovery_ = false;
    return;
  }
  // 1:1 P2P may still be connecting — never convert that into a group-relay timeout leave.
  auto joined = sessions_.CountJoined(call_id);
  if (joined && *joined < 3 && media_.IsActive() && media_.ActiveCallId() == call_id &&
      !media_.IsSfuMode()) {
    ClearSfuAttachWait();
    awaiting_sfu_recovery_ = false;
    return;
  }
  if (util::NowUnixMs() < sfu_attach_wait_deadline_ms_) {
    return;
  }
  ClearSfuAttachWait();
  awaiting_sfu_recovery_ = false;
  last_media_error_ = "No media relay available — group call needs a media_relay hop";
  log().warning << "SFU attach wait timed out call_id=" << call_id;
  (void)LeaveCall(call_id);
}

void CallSessionManager::EjectParticipantAfterMigrateFailure(const std::string& call_id,
                                                             const std::string& identity,
                                                             const std::string& reason) {
  if (identity.empty()) {
    return;
  }
  last_media_error_ = reason;
  log().warning << "Ejecting " << identity << " after soft-migrate failure: " << reason;

  CallLeaveDetail leave;
  leave.call_id = call_id;
  leave.identity = identity;
  auto detail = CallControlCodec::EncodeLeave(leave);
  if (detail) {
    // Notify joiner first while still Joined so they clear chrome.
    (void)SendCallDirectMessage(identity, CallControlType::CallLeave, *detail, "Left the call");
  }

  CallParticipant participant;
  participant.call_id = call_id;
  participant.identity = identity;
  participant.state = CallParticipantState::Left;
  participant.left_at = util::NowUnixMs();
  (void)sessions_.UpsertParticipant(participant);

  if (detail) {
    auto local = LocalRelayIdentity();
    if (local) {
      // Other peers (ejectee already Left → skipped by FanOut).
      (void)FanOutToJoined(call_id, CallControlType::CallLeave, *detail, "Left the call", *local);
    }
  }
  NotifyRingChanged();
}

Roe<std::optional<CallSession>> CallSessionManager::SessionForCall(const std::string& call_id) const {
  return sessions_.LoadSession(call_id);
}

void CallSessionManager::SweepExpiredInvites() {
  auto local = LocalRelayIdentity();
  if (!local) {
    return;
  }
  auto pending = sessions_.ListPendingInvitesForInvitee(*local);
  if (!pending) {
    return;
  }
  const int64_t now = util::NowUnixMs();
  bool changed = false;
  for (PendingCallInvite& invite : *pending) {
    if (invite.status != "pending") {
      continue;
    }
    if (CallSessionLogic::IsInviteExpired(invite, now)) {
      (void)sessions_.UpdateInviteStatus(invite.call_id, invite.invitee_identity, "expired");
      CallParticipant participant;
      participant.call_id = invite.call_id;
      participant.identity = invite.invitee_identity;
      participant.state = CallParticipantState::Missed;
      (void)sessions_.UpsertParticipant(participant);
      changed = true;
    }
  }
  if (changed) {
    NotifyRingChanged();
  }
}

void CallSessionManager::AbandonOrphanedCallsAfterRestart() {
  auto local = LocalRelayIdentity();
  if (!local) {
    return;
  }

  // Copy ids first — LeaveCall / EndCallLocal mutate the store.
  std::vector<std::string> joined_calls;
  std::vector<CallSession> other_live;
  if (auto active = sessions_.ListActiveSessions(); active) {
    for (const CallSession& session : *active) {
      auto participant = sessions_.FindParticipant(session.call_id, *local);
      if (participant && participant->has_value() &&
          (*participant)->state == CallParticipantState::Joined) {
        joined_calls.push_back(session.call_id);
      } else {
        other_live.push_back(session);
      }
    }
  }

  for (const std::string& call_id : joined_calls) {
    if (auto left = LeaveCall(call_id); !left) {
      auto session = sessions_.LoadSession(call_id);
      if (session && session->has_value() && (*session)->state != CallSessionState::Ended) {
        (void)EndCallLocal(**session, std::nullopt);
      }
    }
  }
  for (CallSession& session : other_live) {
    if (session.state == CallSessionState::Ended) {
      continue;
    }
    (void)EndCallLocal(session, std::nullopt);
  }

  if (auto pending = sessions_.ListPendingInvitesForInvitee(*local); pending) {
    for (const PendingCallInvite& invite : *pending) {
      if (invite.status != "pending") {
        continue;
      }
      (void)sessions_.UpdateInviteStatus(invite.call_id, invite.invitee_identity, "expired");
      CallParticipant participant;
      participant.call_id = invite.call_id;
      participant.identity = invite.invitee_identity;
      participant.state = CallParticipantState::Missed;
      (void)sessions_.UpsertParticipant(participant);
      auto session = sessions_.LoadSession(invite.call_id);
      if (session && session->has_value() && (*session)->state != CallSessionState::Ended) {
        (void)EndCallLocal(**session, std::nullopt);
      }
    }
  }

  NotifyRingChanged();
}

bool CallSessionManager::MediaAttemptedThisProcess(const std::string& call_id) const {
  return media_attempted_calls_.find(call_id) != media_attempted_calls_.end();
}

Roe<void> CallSessionManager::ApplyInboundControl(ThreadMessage& message, const std::string& sender_identity) {
  auto type = CallControlCodec::ControlTypeFromMessage(message);
  if (!type) {
    return {};
  }
  const nlohmann::json payload = nlohmann::json::parse(message.payload_json, nullptr, false);
  if (!payload.is_object() || !payload.contains("detail") || !payload["detail"].is_string()) {
    return Error("Call control missing detail");
  }
  const std::string detail_json = payload["detail"].get<std::string>();
  auto local = LocalRelayIdentity();
  if (!local) {
    return local.error();
  }

  switch (*type) {
  case CallControlType::CallInvite: {
    auto invite = CallControlCodec::DecodeInvite(detail_json);
    if (!invite) {
      return invite.error();
    }
    // Do not trust caller wall-clock for ring lifetime (clock skew drops live invites while the
    // system message still persists → unread badge, no Accept UI). Re-arm from local receipt time.
    const int64_t now = util::NowUnixMs();
    if (CallSessionLogic::IsInviteExpired(*invite, now)) {
      log().warning << "CallInvite past wire expires_at; re-arming locally call_id=" << invite->call_id
                    << " expires_at=" << (invite->expires_at ? std::to_string(*invite->expires_at) : "none")
                    << " now=" << now;
    }
    PendingCallInvite pending;
    pending.call_id = invite->call_id;
    pending.inviter_identity = invite->inviter_identity.empty() ? sender_identity : invite->inviter_identity;
    // Always key pending rows to local identity so ListPendingInvitesForInvitee matches.
    pending.invitee_identity = *local;
    pending.media_mode = invite->media_mode;
    pending.origin_thread_id = invite->origin_thread_id;
    pending.origin_group_id = invite->origin_group_id;
    pending.sfu_hint = invite->sfu_hint;
    pending.expires_at = now + kDefaultCallInviteTtlMs;
    pending.created_at = message.timestamp > 0 ? message.timestamp : now;
    pending.status = "pending";
    if (auto saved = sessions_.UpsertPendingInvite(pending); !saved) {
      return saved.error();
    }

    CallSession session;
    session.call_id = invite->call_id;
    session.origin_thread_id = invite->origin_thread_id;
    session.origin_group_id = invite->origin_group_id;
    session.media_mode = invite->media_mode;
    session.state = CallSessionState::Ringing;
    session.created_at = pending.created_at;
    session.media_epoch = 1;
    session.sfu_hint = invite->sfu_hint;
    (void)sessions_.UpsertSession(session);

    CallParticipant inviter;
    inviter.call_id = invite->call_id;
    inviter.identity = pending.inviter_identity;
    inviter.state = CallParticipantState::Joined;
    (void)sessions_.UpsertParticipant(inviter);
    CallParticipant self;
    self.call_id = invite->call_id;
    self.identity = *local;
    self.state = CallParticipantState::Ringing;
    (void)sessions_.UpsertParticipant(self);
    NotifyRingChanged();
    return {};
  }
  case CallControlType::CallAccept: {
    auto accept = CallControlCodec::DecodeAccept(detail_json);
    if (!accept) {
      return accept.error();
    }
    const std::string identity = accept->identity.empty() ? sender_identity : accept->identity;
    CallParticipant participant;
    participant.call_id = accept->call_id;
    participant.identity = identity;
    participant.state = CallParticipantState::Joined;
    participant.media.audio_muted = accept->audio_muted;
    participant.media.video_enabled = accept->video_enabled;
    participant.joined_at = util::NowUnixMs();
    if (auto saved = sessions_.UpsertParticipant(participant); !saved) {
      return saved.error();
    }
    auto session = sessions_.LoadSession(accept->call_id);
    if (session && session->has_value() && (*session)->state != CallSessionState::Ended) {
      (*session)->state = CallSessionLogic::TransitionOnRemoteJoined((*session)->state);
      (void)sessions_.UpsertSession(**session);
    }
    (void)sessions_.UpdateInviteStatus(accept->call_id, identity, "accepted");

    if (session && session->has_value()) {
      const uint32_t epoch = (*session)->media_epoch;
      auto key_bytes = media_keys_.LoadEpochKey(accept->call_id, epoch);
      if (key_bytes && key_bytes->has_value()) {
        (void)SendMediaKeyToPeer(accept->call_id, identity, epoch, (*session)->media_key_id, **key_bytes);
      }
      auto joined_after = sessions_.CountJoined(accept->call_id);
      const size_t n_joined = joined_after ? *joined_after : 0;
      if (CallMediaTopology::ShouldUseMediaRelay(n_joined)) {
        media_attempted_calls_.insert(accept->call_id);
        BrowserThread::PostTask(BrowserThreadId::UI, [this, call_id = accept->call_id, identity]() {
          if (auto mig = MaybeSoftMigrateToSfu(call_id); !mig) {
            log().warning << "MaybeSoftMigrateToSfu failed: " << mig.error().message;
            // Keep existing 1:1 P2P; eject the new joiner so they are not stuck Connecting….
            EjectParticipantAfterMigrateFailure(
                call_id, identity,
                mig.error().message.empty()
                    ? "No media relay available — group call needs a media_relay hop"
                    : mig.error().message);
          }
          NotifyRingChanged();
        });
      } else {
        // Ensure 1:1 accept never inherits a stale SFU attach-wait from an earlier attempt.
        ClearSfuAttachWait();
        awaiting_sfu_recovery_ = false;
        ScheduleStartMediaAsOfferer(accept->call_id, identity);
      }
    }

    NotifyRingChanged();
    return {};
  }
  case CallControlType::CallDecline: {
    auto decline = CallControlCodec::DecodeDecline(detail_json);
    if (!decline) {
      return decline.error();
    }
    const std::string identity = decline->identity.empty() ? sender_identity : decline->identity;
    CallParticipant participant;
    participant.call_id = decline->call_id;
    participant.identity = identity;
    participant.state = CallParticipantState::Declined;
    (void)sessions_.UpsertParticipant(participant);
    (void)sessions_.UpdateInviteStatus(decline->call_id, identity, "declined");
    NotifyRingChanged();
    return {};
  }
  case CallControlType::CallLeave: {
    auto leave = CallControlCodec::DecodeLeave(detail_json);
    if (!leave) {
      return leave.error();
    }
    const std::string identity = leave->identity.empty() ? sender_identity : leave->identity;
    CallParticipant participant;
    participant.call_id = leave->call_id;
    participant.identity = identity;
    participant.state = CallParticipantState::Left;
    participant.left_at = util::NowUnixMs();
    (void)sessions_.UpsertParticipant(participant);
    auto joined = sessions_.CountJoined(leave->call_id);
    auto session = sessions_.LoadSession(leave->call_id);
    if (identity == *local) {
      // Ejected after failed soft-migrate (or remote Leave for us): clear local chrome/media.
      ClearSfuAttachWait();
      if (session && session->has_value() && (*session)->state != CallSessionState::Ended) {
        std::optional<int64_t> duration;
        if ((*session)->created_at > 0) {
          duration = util::NowUnixMs() - (*session)->created_at;
        }
        return EndCallLocal(**session, duration);
      }
      StopMediaIfCall(leave->call_id);
      NotifyRingChanged();
      return {};
    }
    if (session && session->has_value()) {
      const size_t remaining = joined ? *joined : 0;
      const CallSessionState next = CallSessionLogic::TransitionOnLeave((*session)->state, remaining);
      if (next == CallSessionState::Ended) {
        std::optional<int64_t> duration;
        if ((*session)->created_at > 0) {
          duration = util::NowUnixMs() - (*session)->created_at;
        }
        return EndCallLocal(**session, duration);
      }
      (*session)->state = next;
      (void)sessions_.UpsertSession(**session);
      (void)MaybeRotateMediaKey(leave->call_id, identity);
    }
    NotifyRingChanged();
    return {};
  }
  case CallControlType::CallRoster: {
    auto roster = CallControlCodec::DecodeRoster(detail_json);
    if (!roster) {
      return roster.error();
    }
    auto session = sessions_.LoadSession(roster->call_id);
    if (session && session->has_value()) {
      (*session)->media_epoch = roster->media_epoch;
      (void)sessions_.UpsertSession(**session);
    }
    for (const CallRosterEntry& entry : roster->participants) {
      CallParticipant participant;
      participant.call_id = roster->call_id;
      participant.identity = entry.identity;
      participant.state = entry.state;
      participant.media.audio_muted = entry.audio_muted;
      participant.media.video_enabled = entry.video_enabled;
      (void)sessions_.UpsertParticipant(participant);
    }
    NotifyRingChanged();
    return {};
  }
  case CallControlType::CallMediaKey: {
    auto key = CallControlCodec::DecodeMediaKey(detail_json);
    if (!key) {
      return key.error();
    }
    auto session = sessions_.LoadSession(key->call_id);
    if (session && session->has_value()) {
      (*session)->media_epoch = key->media_epoch;
      (*session)->media_key_id = key->media_key_id;
      (void)sessions_.UpsertSession(**session);
    }
    if (!key->wrapped_key_b64.empty()) {
      auto session_key = ResolvePeerSessionKey(sender_identity);
      if (session_key) {
        auto unwrapped = CallMediaKeyStore::UnwrapKeyB64(*session_key, key->wrapped_key_b64, key->call_id,
                                                          key->media_epoch, key->media_key_id);
        if (unwrapped) {
          (void)media_keys_.PutEpochKey(key->call_id, key->media_epoch, *unwrapped);
        }
      }
    }
    return {};
  }
  case CallControlType::CallSdp: {
    auto sdp = CallControlCodec::DecodeSdp(detail_json);
    if (!sdp) {
      return sdp.error();
    }
    if (media_.IsSfuMode()) {
      return {};
    }
    auto applied = media_.SetRemoteDescription(sdp->sdp_type, sdp->sdp);
    // Offer beat answerer Start (common for Linux dial → Mac). Ensure bring-up is scheduled.
    const bool is_offer = (sdp->sdp_type == "offer" || sdp->sdp_type == "Offer");
    if (applied && !media_.IsActive() && is_offer) {
      const std::string call_id = sdp->call_id;
      if (!call_id.empty() && media_attempted_calls_.count(call_id) > 0) {
        ScheduleStartMediaAsAnswerer(call_id, sender_identity);
      }
    }
    return applied;
  }
  case CallControlType::CallIce: {
    auto ice = CallControlCodec::DecodeIce(detail_json);
    if (!ice) {
      return ice.error();
    }
    if (media_.IsSfuMode()) {
      return {}; // ignore ICE after soft-migrate
    }
    return media_.AddRemoteIceCandidate(ice->candidate, ice->mid);
  }
  case CallControlType::CallSfuAttach: {
    auto attach = CallControlCodec::DecodeSfuAttach(detail_json);
    if (!attach) {
      return attach.error();
    }
    if (auto ok = AttachLocalToSfu(attach->call_id, *attach); !ok) {
      last_media_error_ = ok.error().message;
      log().warning << "AttachLocalToSfu (inbound) failed: " << ok.error().message;
      ClearSfuAttachWait();
      // Joiner waiting on SFU: leave instead of hanging on Connecting….
      if (!media_.IsActive() || media_.ActiveCallId() != attach->call_id) {
        (void)LeaveCall(attach->call_id);
      }
    }
    NotifyRingChanged();
    return {};
  }
  case CallControlType::CallEnded: {
    auto ended = CallControlCodec::DecodeEnded(detail_json);
    if (!ended) {
      return ended.error();
    }
    auto session = sessions_.LoadSession(ended->call_id);
    if (session && session->has_value() && (*session)->state != CallSessionState::Ended) {
      return EndCallLocal(**session, ended->duration_ms);
    }
    return {};
  }
  case CallControlType::CallStarted:
    return {};
  }
  return {};
}

uint32_t CallSessionManager::PublisherStreamIdForLocal() const {
  auto local = LocalRelayIdentity();
  if (!local) {
    return 1;
  }
  uint32_t h = 2166136261u;
  for (unsigned char c : *local) {
    h ^= c;
    h *= 16777619u;
  }
  return h == 0 ? 1u : h;
}

void CallSessionManager::RefreshAdaptation(const std::string& /*call_id*/) {
  CallAdaptationInput in;
  in.camera_user_wants = true; // gate applied when user toggles camera
  in.muted = media_.IsMuted();
  in.per_user_up_bps = 0; // unbounded until quote budgets wired into adaptation
  in.allow_video_hi = false;
  media_.ApplyAdaptation(CallMediaAdaptation::Evaluate(in));
}

Roe<void> CallSessionManager::MaybeSoftMigrateToSfu(const std::string& call_id) {
  if (sfu_attached_ && media_.IsSfuMode() && media_.ActiveCallId() == call_id) {
    return {};
  }
  if (!relay_deps_.relay || !relay_deps_.sessions) {
    return Error("media_relay not available");
  }

  auto local = LocalRelayIdentity();
  if (!local) {
    return local.error();
  }
  auto participants = sessions_.ListParticipants(call_id);
  if (!participants) {
    return participants.error();
  }
  std::vector<std::string> joined_ids;
  for (const CallParticipant& p : *participants) {
    if (p.state == CallParticipantState::Joined) {
      joined_ids.push_back(p.identity);
    }
  }
  const auto coordinator = CallSessionLogic::SelectEpochCoordinator(joined_ids);
  if (!coordinator || *coordinator != *local) {
    // Non-coordinator waits for CallSfuAttach from coordinator.
    return {};
  }

  auto ranked = RankedMediaHopCandidates();
  if (ranked.empty()) {
    return Error("no media_relay hop candidates");
  }

  MediaRelayQuoteRequest qreq;
  qreq.call_id = call_id;
  qreq.participants = static_cast<int>(joined_ids.size());
  qreq.want_up_bps = CallMediaAdaptation::kDefaultAudioBps + CallMediaAdaptation::kDefaultVideoLoBps;
  qreq.want_down_bps = qreq.want_up_bps * std::max(1, static_cast<int>(joined_ids.size()) - 1);

  std::string last_err = "all hops failed";
  for (const MeshHopCandidate& hop : ranked) {
    if (!hop.multiaddr.empty()) {
      (void)relay_deps_.sessions->RegisterEndpoint(hop.peer_id, hop.multiaddr);
      relay_deps_.sessions->ClearDialBackoff(hop.peer_id);
    }
    if (!relay_deps_.sessions->IsDialable(hop.peer_id)) {
      last_err = "hop not dialable: " + hop.peer_id;
      continue;
    }
    auto quote = relay_deps_.relay->RequestQuote(hop.peer_id, qreq, 8000);
    if (!quote || !quote->ok) {
      last_err = quote ? quote->error : quote.error().message;
      continue;
    }

    CallSfuAttachDetail attach;
    attach.call_id = call_id;
    attach.hop_peer_id = hop.peer_id;
    attach.hop_multiaddr = hop.multiaddr;
    attach.quote_id = quote->quote_id;
    attach.publisher_stream_id = PublisherStreamIdForLocal();

    if (auto attached = AttachLocalToSfu(call_id, attach); !attached) {
      last_err = attached.error().message;
      continue;
    }

    auto session = sessions_.LoadSession(call_id);
    if (session && session->has_value()) {
      (*session)->sfu_hint = hop.peer_id;
      (void)sessions_.UpsertSession(**session);
    }

    auto encoded = CallControlCodec::EncodeSfuAttach(attach);
    if (encoded) {
      (void)FanOutToJoined(call_id, CallControlType::CallSfuAttach, *encoded, "Call SFU attach", *local);
    }
    return {};
  }
  return Error(last_err);
}

Roe<void> CallSessionManager::AttachLocalToSfu(const std::string& call_id, const CallSfuAttachDetail& attach) {
  if (!relay_deps_.relay || !relay_deps_.sessions) {
    return Error("media_relay not available");
  }
  if (attach.hop_peer_id.empty()) {
    return Error("missing hop_peer_id");
  }
  if (!attach.hop_multiaddr.empty()) {
    (void)relay_deps_.sessions->RegisterEndpoint(attach.hop_peer_id, attach.hop_multiaddr);
    relay_deps_.sessions->ClearDialBackoff(attach.hop_peer_id);
  }
  if (!relay_deps_.sessions->IsDialable(attach.hop_peer_id)) {
    return Error("hop not dialable");
  }

  MediaRelayQuoteRequest qreq;
  qreq.call_id = call_id;
  auto joined = sessions_.CountJoined(call_id);
  qreq.participants = joined ? static_cast<int>(*joined) : 2;
  qreq.want_up_bps = CallMediaAdaptation::kDefaultAudioBps + CallMediaAdaptation::kDefaultVideoLoBps;
  qreq.want_down_bps = qreq.want_up_bps * std::max(1, qreq.participants - 1);
  auto quote = relay_deps_.relay->RequestQuote(attach.hop_peer_id, qreq, 8000);
  if (!quote || !quote->ok) {
    return Error(quote ? quote->error : quote.error().message);
  }
  CallAdaptationInput in;
  in.per_user_up_bps = quote->a_up_bps;
  in.camera_user_wants = false;
  media_.ApplyAdaptation(CallMediaAdaptation::Evaluate(in));

  local_publisher_stream_id_ = PublisherStreamIdForLocal();

  auto attach_res = relay_deps_.relay->AcceptAndAttach(
      attach.hop_peer_id, quote->quote_id, call_id, call_id,
      [this](MediaDataFrame frame) {
        CallMediaEngine::SfuPacket pkt;
        pkt.channel_id = frame.channel_id;
        pkt.seq = frame.seq;
        pkt.mark = frame.mark;
        pkt.payload = std::move(frame.payload);
        media_.OnSfuPacket(pkt);
      },
      8000);
  if (!attach_res || !attach_res->ok) {
    return Error(attach_res ? attach_res->error : attach_res.error().message);
  }

  auto participants = sessions_.ListParticipants(call_id);
  if (participants) {
    auto local = LocalRelayIdentity();
    for (const CallParticipant& p : *participants) {
      if (p.state != CallParticipantState::Joined) {
        continue;
      }
      if (local && p.identity == *local) {
        continue;
      }
      uint32_t h = 2166136261u;
      for (unsigned char c : p.identity) {
        h ^= c;
        h *= 16777619u;
      }
      const uint32_t stream = h == 0 ? 1u : h;
      (void)relay_deps_.relay->Subscribe(stream, 0);
      (void)relay_deps_.relay->Subscribe(stream, 1);
    }
  }

  const uint32_t pub = local_publisher_stream_id_;
  media_attempted_calls_.insert(call_id);
  media_call_id_ = call_id;
  auto started = media_.StartSfu(call_id, [this, pub](const CallMediaEngine::SfuPacket& pkt) {
    if (!relay_deps_.relay) {
      return;
    }
    MediaDataFrame frame;
    frame.stream_id = pub;
    frame.channel_id = pkt.channel_id;
    frame.channel_type =
        pkt.channel_id == 0 ? MediaChannelType::ReliableOrdered : MediaChannelType::LatestLossy;
    frame.seq = pkt.seq;
    frame.mark = pkt.mark;
    frame.payload = pkt.payload;
    (void)relay_deps_.relay->SendFrame(frame);
  });
  if (!started) {
    relay_deps_.relay->Detach();
    return started.error();
  }

  sfu_attached_ = true;
  awaiting_sfu_recovery_ = false;
  media_peer_identity_.clear();
  ClearSfuAttachWait();
  RefreshAdaptation(call_id);
  return {};
}

} // namespace pbr
