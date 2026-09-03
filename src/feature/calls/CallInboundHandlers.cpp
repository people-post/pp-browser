#include "feature/calls/CallSessionManager.h"

#include "domain/messaging/CallSessionLogic.h"
#include "domain/messaging/InitiationPricing.h"
#include "domain/messaging/PeerCapsLogic.h"
#include "domain/people/ContactIdentity.h"
#include "domain/people/ContactJson.h"
#include "domain/people/ContactTypes.h"
#include "foundation/runtime/AppRuntime.h"
#include "common/Utilities.h"
#include "common/PbrCompat.h"

namespace pbr {

namespace {

void NoteCapsForIdentity(CallSessionManager& sessions, ContactsStore& contacts,
                         const std::string& identity, const CallPeerCaps& caps,
                         const std::vector<std::string>& listen_multiaddrs) {
  if (!caps.present && listen_multiaddrs.empty()) {
    return;
  }
  std::vector<std::string> peer_ids = PeerIdsFromListenMultiaddrs(listen_multiaddrs);
  if (peer_ids.empty() && !identity.empty()) {
    if (auto found = contacts.FindByIdentity(identity, ContactIdKind::Account);
        found && found->has_value()) {
      const std::string peer_id = PeerIdFromContact(**found);
      if (!peer_id.empty()) {
        peer_ids.push_back(peer_id);
      }
    }
  }
  for (const std::string& peer_id : peer_ids) {
    if (caps.present) {
      sessions.NotePeerMediaRelayCap(peer_id, caps.media_relay);
    }
    if (IsAccountIdentityValue(identity)) {
      sessions.NoteMeshPeerIdForRelay(identity, peer_id);
    }
  }
}

void PrefetchReach(const CallSessionManager::PrefetchPeerReachFn& fn, const std::string& identity) {
  if (!identity.empty() && fn) {
    fn(identity);
  }
}

} // namespace

Roe<void> CallSessionManager::HandleInboundInvite(const std::string& detail_json,
                                                  const std::string& sender_identity,
                                                  const ThreadMessage& message,
                                                  const std::optional<int64_t> relay_created_at_ms,
                                                  const std::optional<int64_t> relay_server_time_ms,
                                                  const std::string& local_identity) {
  auto invite = CallControlCodec::DecodeInvite(detail_json);
  if (!invite) {
    return invite.error();
  }
  // Already joined this call — ignore redelivered / duplicate invites (would demote to Ringing
  // and flicker ring chrome over the in-call banner).
  if (auto existing = sessions_.FindParticipant(invite->call_id, local_identity);
      existing && existing->has_value() && (*existing)->state == CallParticipantState::Joined) {
    log().info << "CallInvite ignored; already joined call_id=" << invite->call_id;
    return {};
  }
  // P001: when we charge (local floor > 0), auto-reject offers below floor.
  if (initiation_billing_) {
    int64_t local_floor = 0;
    if (auto id = identity_.Get()) {
      local_floor = id->initiation_floor;
    }
    if (local_floor > 0) {
      if (auto ok = InitiationPricing::CheckOfferAgainstFloor(invite->offer_amount_minor, local_floor); !ok) {
        log().info << "CallInvite rejected offer_too_low call_id=" << invite->call_id
                   << " offer=" << invite->offer_amount_minor << " floor=" << local_floor;
        CallDeclineDetail decline;
        decline.call_id = invite->call_id;
        decline.identity = local_identity;
        if (auto encoded = CallControlCodec::EncodeDecline(decline)) {
          (void)SendCallDirectMessage(sender_identity, CallControlType::CallDecline, *encoded,
                                      "Call declined (offer too low)");
        }
        return {};
      }
      (void)initiation_billing_->MarkOffered(sender_identity, invite->offer_amount_minor, local_floor);
    }
  }
  const int64_t now = util::NowUnixMs();
  if (CallSessionLogic::ShouldDropStaleInvite(*invite, now, relay_created_at_ms, relay_server_time_ms)) {
    log().warning << "CallInvite dropped as stale call_id=" << invite->call_id
                  << " expires_at=" << (invite->expires_at ? std::to_string(*invite->expires_at) : "none")
                  << " now=" << now
                  << " relay_created=" << (relay_created_at_ms ? std::to_string(*relay_created_at_ms) : "none")
                  << " relay_now=" << (relay_server_time_ms ? std::to_string(*relay_server_time_ms) : "none");
    return {};
  }
  // Near-live invite: re-arm ring TTL from local receipt (skew-safe). Relay-age gate already
  // dropped long-backlogged inbox rows when create/now samples were present.
  if (CallSessionLogic::IsInviteExpired(*invite, now)) {
    log().info << "CallInvite past wire expires_at; re-arming locally call_id=" << invite->call_id
                  << " expires_at=" << (invite->expires_at ? std::to_string(*invite->expires_at) : "none")
                  << " now=" << now;
  }
  PendingCallInvite pending;
  pending.call_id = invite->call_id;
  pending.inviter_identity = invite->inviter_identity.empty() ? sender_identity : invite->inviter_identity;
  // Always key pending rows to local identity so ListPendingInvitesForInvitee matches.
  pending.invitee_identity = local_identity;
  pending.media_mode = invite->media_mode;
  pending.video_allowed = CallSessionLogic::VideoAllowedFromInvite(*invite);
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
  session.video_allowed = CallSessionLogic::VideoAllowedFromInvite(*invite);
  session.state = CallSessionState::Ringing;
  session.created_at = pending.created_at;
  session.media_epoch = invite->media_epoch > 0 ? invite->media_epoch : 1;
  session.media_key_id = invite->media_key_id;
  session.sfu_hint = invite->sfu_hint;
  (void)sessions_.UpsertSession(session);

  // Media key embedded in invite (preferred); CallMediaKey message remains a backup.
  if (!invite->wrapped_key_b64.empty()) {
    if (auto session_key = ResolvePeerSessionKey(sender_identity)) {
      auto unwrapped = CallMediaKeyStore::UnwrapKeyB64(*session_key, invite->wrapped_key_b64, invite->call_id,
                                                       session.media_epoch, invite->media_key_id);
      if (unwrapped) {
        if (auto put = media_keys_.PutEpochKey(invite->call_id, session.media_epoch, *unwrapped); put) {
          log().info << "CallInvite embedded media key stored call_id=" << invite->call_id
                        << " epoch=" << session.media_epoch;
          if (call_media_bridge_) {
            call_media_bridge_->OnMediaKeyReady(invite->call_id);
          }
        } else {
          log().warning << "CallInvite media key store failed: " << put.error().message;
        }
      } else {
        log().warning << "CallInvite media key unwrap failed: " << unwrapped.error().message;
      }
    } else {
      log().warning << "CallInvite media key missing peer session key from=" << sender_identity;
    }
  }

  // Seed full roster from invite when present; fall back to inviter+self.
  if (!invite->participants.empty()) {
    for (const CallRosterEntry& entry : invite->participants) {
      if (entry.identity.empty()) {
        continue;
      }
      CallParticipant row;
      row.call_id = invite->call_id;
      row.identity = entry.identity;
      row.state = entry.state;
      row.media.audio_muted = entry.audio_muted;
      row.media.video_enabled = entry.video_enabled;
      if (entry.identity == local_identity) {
        row.state = CallParticipantState::Ringing;
      } else if (entry.identity == pending.inviter_identity &&
                 row.state != CallParticipantState::Joined) {
        row.state = CallParticipantState::Joined;
      }
      if (row.state == CallParticipantState::Joined) {
        // Prefer earlier stamp than late acceptors so soft-migrate initiator detection works.
        row.joined_at = session.created_at > 0 ? session.created_at : 1;
      }
      (void)sessions_.UpsertParticipant(row);
    }
  }
  CallParticipant inviter;
  inviter.call_id = invite->call_id;
  inviter.identity = pending.inviter_identity;
  inviter.state = CallParticipantState::Joined;
  inviter.joined_at = session.created_at > 0 ? session.created_at : 1;
  (void)sessions_.UpsertParticipant(inviter);
  CallParticipant self;
  self.call_id = invite->call_id;
  self.identity = local_identity;
  self.state = CallParticipantState::Ringing;
  (void)sessions_.UpsertParticipant(self);
  if (register_peer_listen_multiaddrs_ && !invite->listen_multiaddrs.empty()) {
    register_peer_listen_multiaddrs_(pending.inviter_identity, invite->listen_multiaddrs);
  }
  if (!invite->libp2p_peer_id.empty()) {
    NoteMeshPeerIdForRelay(pending.inviter_identity, invite->libp2p_peer_id);
  }
  NoteCapsForIdentity(*this, contacts_, pending.inviter_identity, invite->caps, invite->listen_multiaddrs);
  PrefetchReach(prefetch_reach_, pending.inviter_identity);
  NotifyRingChanged();
  return {};
}

Roe<void> CallSessionManager::HandleInboundAccept(const std::string& detail_json,
                                                  const std::string& sender_identity,
                                                  const std::string& local_identity) {
  auto accept = CallControlCodec::DecodeAccept(detail_json);
  if (!accept) {
    return accept.error();
  }
  const std::string identity = accept->identity.empty() ? sender_identity : accept->identity;
  log().info << "Inbound CallAccept call_id=" << accept->call_id << " from=" << identity;
  if (register_peer_listen_multiaddrs_ && !accept->listen_multiaddrs.empty()) {
    register_peer_listen_multiaddrs_(identity, accept->listen_multiaddrs);
  }
  if (!accept->libp2p_peer_id.empty()) {
    NoteMeshPeerIdForRelay(identity, accept->libp2p_peer_id);
  }
  NoteCapsForIdentity(*this, contacts_, identity, accept->caps, accept->listen_multiaddrs);
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
      if (auto keyed = SendMediaKeyToPeer(accept->call_id, identity, epoch, (*session)->media_key_id, **key_bytes);
          !keyed) {
        log().warning << "CallMediaKey send failed call_id=" << accept->call_id
                      << " peer=" << identity << " err=" << keyed.error().message;
      } else {
        log().info << "CallMediaKey sent call_id=" << accept->call_id << " peer=" << identity
                      << " epoch=" << epoch;
      }
    } else {
      log().warning << "CallMediaKey missing locally on CallAccept call_id=" << accept->call_id
                    << " epoch=" << epoch;
    }
    auto joined_after = sessions_.CountJoined(accept->call_id);
    const size_t n_joined = joined_after ? *joined_after : 0;
    if (!topology_.OnRemoteAcceptJoined(accept->call_id, n_joined, identity)) {
      ScheduleStartDirectMedia(accept->call_id, identity, true);
    }
    // Prefetch + roster fan-out after media kickoff — avoid starving MediaKey/Connect on IO.
    const std::string accept_call_id = accept->call_id;
    const std::string accept_peer = identity;
    AppRuntime::PostWorkerNormal([this, accept_call_id, accept_peer, local = local_identity]() {
      PrefetchReach(prefetch_reach_, accept_peer);
      if (auto roster = BuildRosterDetail(accept_call_id); roster) {
        if (auto roster_json = CallControlCodec::EncodeRoster(*roster); roster_json) {
          (void)FanOutToJoinedAndRinging(accept_call_id, CallControlType::CallRoster, *roster_json, "Call roster",
                                         local);
        }
      }
    });
  }

  NotifyRingChanged();
  return {};
}

Roe<void> CallSessionManager::HandleInboundDecline(const std::string& detail_json,
                                                   const std::string& sender_identity) {
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

Roe<void> CallSessionManager::HandleInboundLeave(const std::string& detail_json,
                                                 const std::string& sender_identity,
                                                 const std::string& local_identity) {
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
  if (identity == local_identity) {
    // Ejected after failed soft-migrate (or remote Leave for us): clear local chrome/media.
    topology_.ClearSfuAttachWait();
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

Roe<void> CallSessionManager::HandleInboundRoster(const std::string& detail_json) {
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
    if (entry.identity.empty()) {
      continue;
    }
    // Do not resurrect Left/Declined peers from a stale roster fan-out (blocks re-invite).
    if (entry.state == CallParticipantState::Joined || entry.state == CallParticipantState::Ringing ||
        entry.state == CallParticipantState::Invited) {
      if (auto existing = sessions_.FindParticipant(roster->call_id, entry.identity);
          existing && existing->has_value()) {
        const CallParticipantState prior = (*existing)->state;
        if (prior == CallParticipantState::Left || prior == CallParticipantState::Declined ||
            prior == CallParticipantState::Missed) {
          continue;
        }
        // Joiner BuildRoster still lists earlier invitees as Ringing. Applying that must not
        // demote peers who already Accepted (SoftMigrate N≥3 / in-call UI depend on Joined).
        if (prior == CallParticipantState::Joined &&
            (entry.state == CallParticipantState::Ringing ||
             entry.state == CallParticipantState::Invited)) {
          CallParticipant keep = **existing;
          keep.media.audio_muted = entry.audio_muted;
          keep.media.video_enabled = entry.video_enabled;
          (void)sessions_.UpsertParticipant(keep);
          continue;
        }
      }
    }
    CallParticipant participant;
    participant.call_id = roster->call_id;
    participant.identity = entry.identity;
    participant.state = entry.state;
    participant.media.audio_muted = entry.audio_muted;
    participant.media.video_enabled = entry.video_enabled;
    participant.joined_at = entry.joined_at;
    (void)sessions_.UpsertParticipant(participant);
  }
  // Mid-call invite: CallAccept only reaches the inviter; initiator SoftMigrates when roster
  // shows N≥3 (V021 sticky initiator / V022 payer).
  if (auto joined = sessions_.CountJoined(roster->call_id); joined) {
    topology_.OnJoinedCountObserved(roster->call_id, *joined);
  }
  NotifyRingChanged();
  return {};
}

Roe<void> CallSessionManager::HandleInboundMediaKey(const std::string& detail_json,
                                                    const std::string& sender_identity) {
  auto key = CallControlCodec::DecodeMediaKey(detail_json);
  if (!key) {
    return key.error();
  }
  log().info << "Inbound CallMediaKey call_id=" << key->call_id << " epoch=" << key->media_epoch
                << " from=" << sender_identity;
  auto session = sessions_.LoadSession(key->call_id);
  if (session && session->has_value()) {
    (*session)->media_epoch = key->media_epoch;
    (*session)->media_key_id = key->media_key_id;
    (void)sessions_.UpsertSession(**session);
  }
  bool stored = false;
  if (!key->wrapped_key_b64.empty()) {
    auto session_key = ResolvePeerSessionKey(sender_identity);
    if (session_key) {
      auto unwrapped = CallMediaKeyStore::UnwrapKeyB64(*session_key, key->wrapped_key_b64, key->call_id,
                                                        key->media_epoch, key->media_key_id);
      if (unwrapped) {
        if (auto put = media_keys_.PutEpochKey(key->call_id, key->media_epoch, *unwrapped); put) {
          stored = true;
        } else {
          log().warning << "CallMediaKey store failed: " << put.error().message;
        }
      } else {
        log().warning << "CallMediaKey unwrap failed: " << unwrapped.error().message;
      }
    } else {
      log().warning << "CallMediaKey missing peer session key from=" << sender_identity;
    }
  }
  // Mesh answerer Start waits for epoch key (V015); kick deferred BeginSession.
  if (stored && call_media_bridge_) {
    call_media_bridge_->OnMediaKeyReady(key->call_id);
  }
  return {};
}

Roe<void> CallSessionManager::HandleInboundSfuAttach(const std::string& detail_json) {
  auto attach = CallControlCodec::DecodeSfuAttach(detail_json);
  if (!attach) {
    return attach.error();
  }
  (void)topology_.OnInboundSfuAttach(attach->call_id, *attach);
  NotifyRingChanged();
  return {};
}

Roe<void> CallSessionManager::HandleInboundSfuAttachFailed(const std::string& detail_json,
                                                           const std::string& sender_identity) {
  auto failed = CallControlCodec::DecodeSfuAttachFailed(detail_json);
  if (!failed) {
    return failed.error();
  }
  if (failed->identity.empty()) {
    failed->identity = sender_identity;
  }
  topology_.OnInboundSfuAttachFailed(*failed);
  NotifyRingChanged();
  return {};
}

Roe<void> CallSessionManager::HandleInboundHopRefuse(const std::string& detail_json) {
  auto refused = CallControlCodec::DecodeHopRefuse(detail_json);
  if (!refused) {
    return refused.error();
  }
  topology_.OnInboundHopRefuse(*refused);
  NotifyRingChanged();
  return {};
}

Roe<void> CallSessionManager::HandleInboundVideoRefresh(const std::string& detail_json,
                                                        const std::string& sender_identity) {
  auto refresh = CallControlCodec::DecodeVideoRefresh(detail_json);
  if (!refresh) {
    return refresh.error();
  }
  auto local = LocalRelayIdentity();
  if (!local) {
    return local.error();
  }
  bool sender_joined = false;
  if (auto participants = sessions_.ListParticipants(refresh->call_id); participants) {
    for (const CallParticipant& p : *participants) {
      if (p.identity == sender_identity && p.state == CallParticipantState::Joined) {
        sender_joined = true;
        break;
      }
    }
  }
  if (!CallSessionLogic::ShouldHonorInboundVideoRefresh(refresh->call_id, refresh->identity, sender_identity,
                                                        *local, media_.ActiveCallId(), sender_joined)) {
    return {};
  }
  media_.RequestVideoKeyframe();
  return {};
}

Roe<void> CallSessionManager::HandleInboundEnded(const std::string& detail_json,
                                                 const std::string& local_identity) {
  auto ended = CallControlCodec::DecodeEnded(detail_json);
  if (!ended) {
    return ended.error();
  }
  (void)sessions_.UpdateInviteStatus(ended->call_id, local_identity, "expired");
  auto session = sessions_.LoadSession(ended->call_id);
  if (session && session->has_value() && (*session)->state != CallSessionState::Ended) {
    return EndCallLocal(**session, ended->duration_ms);
  }
  NotifyRingChanged();
  return {};
}

} // namespace pbr
