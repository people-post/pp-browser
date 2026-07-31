#include <stdexcept>
#include "feature/ui/CallController.h"

#include "base/i18n/LocalizationService.h"
#include "base/media/CallMediaEngine.h"
#include "base/messaging/CallTypes.h"
#include "base/people/ContactTypes.h"
#include "base/platform/BrowserThread.h"
#include "base/platform/ILocalNotifier.h"
#include "base/platform/PlatformUserHints.h"
#include "base/platform/ProductBranding.h"
#include "base/ui/ShellTypes.h"
#include "feature/messaging/MessagingHub.h"
#include "feature/ui/CallChromeSync.h"
#include "feature/ui/CallConflictCopy.h"
#include "feature/ui/PeoplePickerController.h"
#include "CallVideoTileRenderer.h"
#include "feature/ui/ShellHost.h"
#include "feature/ui/UserFeedback.h"

#include "common/Utilities.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <string>

namespace pbr {
namespace {

CallChromeLayer CaptureCallChrome(const ShellState& state) {
  return {
      .ring_active = state.call_ring.active,
      .in_call_active = state.call_in_progress.active,
      .ring_pulse = state.call_ring.pulse,
      .in_call_muted = state.call_in_progress.muted,
      .in_call_camera_on = state.call_in_progress.camera_on,
      .in_call_stage_visible = state.call_in_progress.stage_visible,
      .in_call_remote_video = state.call_in_progress.remote_video,
      .in_call_local_preview = state.call_in_progress.local_preview,
      .ring_conflict = state.call_ring.conflict,
      .ring_call_id = state.call_ring.call_id.c_str(),
      .in_call_id = state.call_in_progress.call_id.c_str(),
      .in_call_subtitle = state.call_in_progress.subtitle.c_str(),
      .ring_caller_label = state.call_ring.caller_label.c_str(),
      .ring_media_label = state.call_ring.media_label.c_str(),
      .ring_eyebrow = state.call_ring.eyebrow.c_str(),
      .ring_conflict_hint = state.call_ring.conflict_hint.c_str(),
      .ring_accept_label = state.call_ring.accept_label.c_str(),
      .ring_decline_label = state.call_ring.decline_label.c_str(),
      .in_call_title = state.call_in_progress.title.c_str(),
      .in_call_mic_level = state.call_in_progress.mic_level,
      .in_call_peer_level = state.call_in_progress.peer_level,
      .in_call_mic_hint = state.call_in_progress.mic_hint.c_str(),
      .in_call_peer_hint = state.call_in_progress.peer_hint.c_str(),
      .in_call_elapsed = state.call_in_progress.elapsed.c_str(),
      .in_call_peer_label = state.call_in_progress.peer_label.c_str(),
      .in_call_remote_placeholder = state.call_in_progress.remote_placeholder.c_str(),
      .in_call_show_roster = state.call_in_progress.show_roster,
      .in_call_show_invite = state.call_in_progress.show_invite,
      .in_call_show_retry = state.call_in_progress.show_retry,
      .in_call_participant_count = state.call_in_progress.participant_count,
      .in_call_status_hint = state.call_in_progress.status_hint.c_str(),
  };
}

int QuantizeAudioLevel(float level) {
  if (level < 0.008f) {
    return 0;
  }
  if (level < 0.03f) {
    return 1;
  }
  if (level < 0.08f) {
    return 2;
  }
  if (level < 0.18f) {
    return 3;
  }
  if (level < 0.35f) {
    return 4;
  }
  return 5;
}

std::string LevelHint(int level, bool remote, bool muted) {
  if (muted && !remote) {
    return Tr("call.level.muted");
  }
  if (level <= 0) {
    return remote ? Tr("call.level.quiet") : Tr("call.level.silent");
  }
  if (level <= 2) {
    return Tr("call.level.speaking");
  }
  return Tr("call.level.loud");
}

std::string ComposeP2pStatusHint(bool missing_mic) {
  const std::map<std::string, std::string> product{{"product", kProductName}};
  std::string hint = Tr(PlatformUserHints::P2pNetworkHintKey(), product);
  if (missing_mic) {
    const std::string mic = Tr(PlatformUserHints::MicBlockedHintKey());
    if (!mic.empty()) {
      if (!hint.empty()) {
        hint += " ";
      }
      hint += mic;
    }
  }
  return hint;
}

std::string ComposeGroupCallStatusHint() {
  return Tr("call.hint.group_media");
}

} // namespace

void CallController::BindMessaging(MessagingHub& messaging) {
  messaging_ = &messaging;
  BindToMessaging();
}

MessagingHub& CallController::Hub() {
  if (!messaging_) {
    throw std::runtime_error("CallController messaging not bound");
  }
  return *messaging_;
}

const MessagingHub& CallController::Hub() const {
  if (!messaging_) {
    throw std::runtime_error("CallController messaging not bound");
  }
  return *messaging_;
}


void CallController::BindToMessaging() {
  if (!messaging_ || !Hub().IsInitialized()) {
    bound_calls_ = nullptr;
    return;
  }
  auto* calls = Hub().Calls();
  if (!calls) {
    bound_calls_ = nullptr;
    return;
  }
  // CallSessionManager is recreated in BuildMessagingStack; rebind when the pointer changes.
  if (bound_calls_ == calls) {
    return;
  }
  calls->SetOnRingChanged([this]() {
    // Ingest may run on IO; shell/RmlUi updates must stay on UI.
    BrowserThread::PostTask(BrowserThreadId::UI, [this]() { RefreshPendingRing(); });
  });
  bound_calls_ = calls;
  // Pick up post-restart abandon / pending ring after stack rebuild.
  RefreshPendingRing();
}

void CallController::Tick() {
  BindToMessaging();
  if (auto* calls = Hub().Calls()) {
    calls->SweepExpiredInvites();
  }
  const int64_t now = util::NowUnixMs();
  auto& ring = ShellHost::Instance().State().call_ring;
  if (ring.active) {
    if (now - last_pulse_toggle_ms_ >= 600) {
      ring.pulse = !ring.pulse;
      last_pulse_toggle_ms_ = now;
      SyncShellState();
    }
  }
  RefreshCallLevels();
  SyncRingtone();
}

void CallController::OnCallWake() {
  // Inbox sync is async; notify once RefreshPendingRing sees the invite.
  pending_call_wake_notify_ = true;
  RefreshPendingRing();
}

void CallController::ClearRing() {
  ringing_call_id_.clear();
  ring_started_ms_ = 0;
  ShellHost::Instance().State().call_ring = {};
  ringtone_.Stop();
}

void CallController::ClearInCall() {
  active_call_id_.clear();
  ShellHost::Instance().State().call_in_progress = {};
  CallVideoTileRenderer::Instance().Clear();
}

void CallController::HideInCallChrome() {
  ShellHost::Instance().State().call_in_progress = {};
  CallVideoTileRenderer::Instance().Clear();
}

void CallController::SyncShellState() {
  const CallChromeLayer next = CaptureCallChrome(ShellHost::Instance().State());
  const CallChromeUpdate update = ClassifyCallChromeUpdate(synced_chrome_, next);
  synced_chrome_ = next;

  if (update == CallChromeUpdate::None) {
    return;
  }
  ShellHost::Instance().DirtyWindow();
}

void CallController::SyncRingtone() {
  const bool should_ring = ShellHost::Instance().State().call_ring.active;
  if (should_ring && !ringtone_.IsPlaying()) {
    ringtone_.Start();
  } else if (!should_ring && ringtone_.IsPlaying()) {
    ringtone_.Stop();
  }
}

std::string CallController::DisplayNameForIdentity(const std::string& identity) const {
  if (identity.empty() || !messaging_) {
    return {};
  }
  if (auto contact = messaging_->Contacts().FindByIdentity(identity, ContactIdKind::RelayUser)) {
    if (*contact) {
      std::string name =
          (*contact)->display_name.empty() ? (*contact)->server_nickname : (*contact)->display_name;
      if (!name.empty()) {
        return name;
      }
    }
  }
  return identity;
}

std::string CallController::FormatElapsed(const int64_t connected_at_ms) {
  if (connected_at_ms <= 0) {
    return {};
  }
  const int64_t sec = std::max<int64_t>(0, (util::NowUnixMs() - connected_at_ms) / 1000);
  const int64_t mins = sec / 60;
  const int64_t secs = sec % 60;
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%lld:%02lld", static_cast<long long>(mins), static_cast<long long>(secs));
  return buf;
}

void CallController::RefreshPendingRing() {
  BindToMessaging();
  auto* calls = Hub().Calls();
  if (!calls) {
    return;
  }

  if (auto media_err = calls->TakeLastMediaError(); media_err && !media_err->empty()) {
    UserFeedback::Fail(*media_err);
  }

  calls->PollPendingSfuAttach();
  calls->PollP2pConnectHealth();

  auto top = calls->TopPendingInvite();
  if (top && top->has_value()) {
    ringing_call_id_ = (*top)->call_id;
    if (ring_started_ms_ == 0) {
      ring_started_ms_ = util::NowUnixMs();
    }

    const std::string caller_label = [&]() {
      std::string name = DisplayNameForIdentity((*top)->inviter_identity);
      if (name.empty()) {
        name = (*top)->inviter_identity;
      }
      return name;
    }();

    bool has_conflict = false;
    bool same_peer = false;
    std::string active_peer_label;
    if (auto active = calls->ActiveLocalCall(); active && active->has_value() &&
        (*active)->call_id != (*top)->call_id) {
      has_conflict = true;
      active_call_id_ = (*active)->call_id;
      if (auto peer = calls->PeerIdentityForCall((*active)->call_id); peer && peer->has_value()) {
        active_peer_label = DisplayNameForIdentity(**peer);
        if (active_peer_label.empty()) {
          active_peer_label = **peer;
        }
        same_peer = (**peer == (*top)->inviter_identity);
      }
    } else {
      active_call_id_.clear();
    }

    const CallConflictCopy copy =
        MakeCallConflictCopy(has_conflict, same_peer, caller_label, active_peer_label);

    // Hide in-call bar while ringing; keep active_call_id_ when conflicting.
    HideInCallChrome();

    auto& ring = ShellHost::Instance().State().call_ring;
    ring.active = true;
    ring.conflict = has_conflict;
    ring.call_id = (*top)->call_id;
    ring.caller_label = caller_label;
    ring.media_label = (*top)->media_mode == CallMediaMode::Video
                           ? Tr("call.ring.incoming_video").c_str()
                           : Tr("call.ring.incoming_voice").c_str();
    ring.eyebrow = copy.eyebrow;
    ring.conflict_hint = copy.hint;
    ring.accept_label = copy.accept_label;
    ring.decline_label = copy.decline_label;
    if (pending_call_wake_notify_) {
      pending_call_wake_notify_ = false;
      const std::string body =
          caller_label.empty()
              ? Tr("call.notify.body_unknown")
              : Tr("call.notify.body_named", {{"name", caller_label}});
      ILocalNotifier::Instance().NotifyIncoming(Tr("call.notify.title"), body, "");
    }
    SyncShellState();
    SyncRingtone();
    return;
  }

  if (auto active = calls->ActiveLocalCall(); active && active->has_value()) {
    active_call_id_ = (*active)->call_id;
    ClearRing();

    // Disk session survived force-quit but media did not — drop stuck "Calling…" chrome.
    if ((*active)->state == CallSessionState::Active && !calls->Media().IsActive() &&
        !calls->MediaAttemptedThisProcess(active_call_id_)) {
      (void)calls->LeaveCall(active_call_id_);
      ClearInCall();
      ClearRing();
      SyncShellState();
      return;
    }

    // Peer ICE failed: keep chrome for Retry/End on 1:1. Group SFU recovery keeps chrome too.
    // Do not auto-LeaveCall on `failed` — that erased the session before the user could retry.
    if (calls->Media().IsActive() && calls->Media().ActiveCallId() == active_call_id_) {
      const std::string media_state = calls->Media().ConnectionState();
      if (media_state == "failed" && !calls->IsAwaitingSfuRecovery() && !calls->Media().IsSfuMode() &&
          !calls->IsP2pConnectFailed()) {
        // State callback may not have marked yet (ordering); ensure UI can show Retry.
        calls->PollP2pConnectHealth();
      }
    }

    auto& in_call = ShellHost::Instance().State().call_in_progress;
    in_call.active = true;
    in_call.call_id = (*active)->call_id;
    in_call.muted = calls->Media().IsMuted();

    const bool is_video = (*active)->media_mode == CallMediaMode::Video;
    int joined_count = 0;
    std::string local_identity;
    if (auto identity = Hub().Identity().Get()) {
      local_identity = identity->relay_user_id;
    }
    if (auto participants = calls->ListJoinedParticipants((*active)->call_id); participants) {
      joined_count = static_cast<int>(participants->size());
      in_call.participant_count = joined_count;
      in_call.show_roster = joined_count > 2 || (*active)->origin_group_id.has_value();
      in_call.show_invite = true;
      in_call.roster.clear();
      in_call.roster.reserve(participants->size());
      for (const CallParticipant& row : *participants) {
        CallRosterParticipantState entry;
        entry.is_local = !local_identity.empty() && row.identity == local_identity;
        std::string name = entry.is_local ? Tr("call.label.you") : DisplayNameForIdentity(row.identity);
        if (!entry.is_local && name.empty()) {
          name = row.identity;
        }
        entry.name = name.c_str();
        entry.audio_muted = row.media.audio_muted;
        entry.video_enabled = row.media.video_enabled;
        in_call.roster.push_back(std::move(entry));
      }
    } else {
      in_call.participant_count = 0;
      in_call.show_roster = false;
      in_call.show_invite = true;
      in_call.roster.clear();
    }

    if (in_call.show_roster) {
      in_call.title = is_video ? Tr("call.title.group_video").c_str() : Tr("call.title.group_voice").c_str();
    } else {
      in_call.title = is_video ? Tr("call.title.video").c_str() : Tr("call.title.voice").c_str();
    }

    std::string peer_label = Tr("call.label.them");
    if (auto peer = calls->PeerIdentityForCall((*active)->call_id); peer && peer->has_value()) {
      const std::string name = DisplayNameForIdentity(**peer);
      if (!name.empty()) {
        peer_label = name;
      }
    }
    in_call.peer_label = in_call.show_roster ? Tr("call.label.others").c_str() : peer_label.c_str();

    const bool p2p_failed = calls->IsP2pConnectFailed();
    const bool group_call_context = (*active)->origin_group_id.has_value() || joined_count > 2 ||
                                    calls->IsAwaitingSfuRecovery() || calls->Media().IsSfuMode();
    in_call.show_retry = p2p_failed && !calls->IsAwaitingSfuRecovery() && !calls->Media().IsSfuMode();
    if (p2p_failed) {
      in_call.status_hint =
          group_call_context ? ComposeGroupCallStatusHint().c_str()
                             : ComposeP2pStatusHint(calls->P2pConnectMissingMic()).c_str();
      in_call.show_invite = false;
    } else {
      in_call.status_hint = {};
    }

    if (calls->Media().IsConnected()) {
      in_call.elapsed = FormatElapsed(calls->Media().ConnectedAtMs());
      in_call.subtitle = in_call.elapsed.empty() ? Tr("call.status.connected").c_str() : in_call.elapsed;
      in_call.show_retry = false;
      in_call.status_hint = {};
    } else if (p2p_failed) {
      in_call.elapsed = {};
      in_call.subtitle = Tr("call.status.couldnt_connect").c_str();
    } else if (!calls->Media().IsActive()) {
      in_call.elapsed = {};
      in_call.subtitle = Tr("call.status.calling").c_str();
    } else {
      in_call.elapsed = {};
      const std::string state = calls->Media().ConnectionState();
      if (calls->IsAwaitingSfuRecovery()) {
        in_call.subtitle = Tr("call.status.reconnecting").c_str();
      } else if (state == "connecting" || state.empty() || state == "new") {
        in_call.subtitle = Tr("call.status.connecting").c_str();
      } else if (state == "disconnected") {
        in_call.subtitle = Tr("call.status.reconnecting").c_str();
      } else if (state == "failed") {
        in_call.subtitle = Tr("call.status.couldnt_connect").c_str();
      } else if (state == "closed") {
        in_call.subtitle = Tr("call.status.connecting").c_str();
      } else {
        in_call.subtitle = state;
      }
    }
    if (in_call.show_roster && joined_count > 0 && calls->Media().IsConnected()) {
      in_call.subtitle =
          Tr("call.participants_elapsed",
             {{"count", std::to_string(joined_count)}, {"elapsed", std::string(in_call.elapsed.c_str())}})
              .c_str();
    }
    ApplyAudioLevels(calls->Media());
    SyncShellState();
    return;
  }

  ClearInCall();
  ClearRing();
  SyncShellState();
}

bool CallController::StartCall(const std::string& thread_id, const bool video) {
  BindToMessaging();
  auto* calls = Hub().Calls();
  if (!calls) {
    UserFeedback::Fail(Tr("call.error.unavailable"));
    return false;
  }
  auto thread = Hub().Store().GetThread(thread_id);
  if (!thread || !*thread) {
    UserFeedback::Fail(Tr("call.error.thread_not_found"));
    return false;
  }
  if ((*thread)->kind == ThreadKind::Group) {
    OpenGroupCallPicker(thread_id, video);
    return true;
  }
  if ((*thread)->kind != ThreadKind::Direct) {
    UserFeedback::Fail(Tr("call.error.wrong_thread_type"));
    return false;
  }
  if ((*thread)->peer_identity_value.empty()) {
    UserFeedback::Fail(Tr("call.error.missing_peer"));
    return false;
  }
  return StartCallWithInvitees(thread_id, video, {(*thread)->peer_identity_value});
}

bool CallController::StartCallWithInvitees(const std::string& thread_id, const bool video,
                                           const std::vector<std::string>& invitee_identities) {
  BindToMessaging();
  auto* calls = Hub().Calls();
  if (!calls) {
    UserFeedback::Fail(Tr("call.error.unavailable"));
    return false;
  }
  if (invitee_identities.empty()) {
    UserFeedback::Fail(Tr("call.error.select_person"));
    return false;
  }
  auto started =
      calls->StartCall(thread_id, video ? CallMediaMode::Video : CallMediaMode::Voice, invitee_identities);
  if (!started) {
    UserFeedback::Fail(started.error().message);
    return false;
  }
  active_call_id_ = started->call_id;
  RefreshPendingRing();
  return true;
}

void CallController::OpenGroupCallPicker(const std::string& thread_id, const bool video) {
  PeoplePickerController::Instance().OpenForGroupCall(thread_id, video);
}

void CallController::OpenMidCallInvitePicker() {
  if (active_call_id_.empty()) {
    UserFeedback::Fail(Tr("call.error.no_active"));
    return;
  }
  PeoplePickerController::Instance().OpenForCallAddGuest(active_call_id_);
}

void CallController::InviteIdentitiesToActiveCall(const std::vector<std::string>& invitee_identities) {
  BindToMessaging();
  auto* calls = Hub().Calls();
  if (!calls || active_call_id_.empty()) {
    UserFeedback::Fail(Tr("call.error.no_active"));
    return;
  }
  int invited = 0;
  for (const std::string& identity : invitee_identities) {
    if (identity.empty()) {
      continue;
    }
    if (auto ok = calls->InviteParticipant(active_call_id_, identity); ok) {
      ++invited;
    } else {
      UserFeedback::Fail(ok.error().message);
      break;
    }
  }
  if (invited > 0) {
    RefreshPendingRing();
  }
}

bool CallController::StartVoiceCall(const std::string& thread_id) {
  return StartCall(thread_id, false);
}

bool CallController::StartVideoCall(const std::string& thread_id) {
  return StartCall(thread_id, true);
}

void CallController::AcceptIncoming() {
  BindToMessaging();
  auto* calls = Hub().Calls();
  if (!calls || ringing_call_id_.empty()) {
    return;
  }
  ringtone_.Stop();
  // End conflicting outbound/in-call first; AcceptInvite also enforces this.
  if (!active_call_id_.empty() && active_call_id_ != ringing_call_id_) {
    if (auto left = calls->LeaveCall(active_call_id_); !left) {
      UserFeedback::Fail(left.error().message);
      RefreshPendingRing();
      return;
    }
    active_call_id_.clear();
  }
  // AcceptInvite only does signaling; media starts on a later UI task so this
  // Rml callback can return and dismiss the ring dialog immediately.
  if (auto accepted = calls->AcceptInvite(ringing_call_id_); !accepted) {
    UserFeedback::Fail(accepted.error().message);
  }
  RefreshPendingRing();
}

void CallController::DeclineIncoming() {
  BindToMessaging();
  auto* calls = Hub().Calls();
  if (!calls || ringing_call_id_.empty()) {
    return;
  }
  ringtone_.Stop();
  (void)calls->DeclineInvite(ringing_call_id_);
  RefreshPendingRing();
}

void CallController::LeaveActive() {
  BindToMessaging();
  auto* calls = Hub().Calls();
  if (!calls || active_call_id_.empty()) {
    return;
  }
  (void)calls->LeaveCall(active_call_id_);
  RefreshPendingRing();
}

void CallController::RetryConnect() {
  BindToMessaging();
  auto* calls = Hub().Calls();
  if (!calls || active_call_id_.empty()) {
    return;
  }
  if (auto retried = calls->RetryP2pMedia(active_call_id_); !retried) {
    UserFeedback::Fail(retried.error().message);
  }
  RefreshPendingRing();
}

void CallController::ToggleMute() {
  BindToMessaging();
  auto* calls = Hub().Calls();
  if (!calls || !calls->Media().IsActive()) {
    return;
  }
  if (auto muted = calls->SetLocalAudioMuted(!calls->Media().IsMuted()); !muted) {
    UserFeedback::Fail(muted.error().message);
  }
  RefreshPendingRing();
}

void CallController::ToggleCamera() {
  BindToMessaging();
  auto* calls = Hub().Calls();
  if (!calls || !calls->Media().IsActive()) {
    return;
  }
  const bool next = !calls->Media().IsCameraEnabled();
  if (auto cam = calls->SetLocalVideoEnabled(next); !cam) {
    UserFeedback::Fail(cam.error().message);
  }
  RefreshPendingRing();
}

void CallController::ApplyAudioLevels(CallMediaEngine& media) {
  media.RefreshRemoteVideoHealth();

  auto& in_call = ShellHost::Instance().State().call_in_progress;
  const bool muted = media.IsMuted();
  in_call.muted = muted;
  in_call.camera_on = media.IsCameraEnabled();

  bool peer_camera_on = false;
  bool have_peer_video_flag = false;
  auto* calls = Hub().Calls();
  if (calls && !active_call_id_.empty()) {
    if (auto peer_video = calls->PeerVideoEnabledForCall(active_call_id_);
        peer_video && peer_video->has_value()) {
      peer_camera_on = **peer_video;
      have_peer_video_flag = true;
    }
  }
  if (have_peer_video_flag && !peer_camera_on) {
    media.ClearRemoteVideo();
  }

  const bool stalling = media.IsRemoteVideoStalling();
  const std::string conn = media.ConnectionState();
  const bool expect_remote_video = !have_peer_video_flag || peer_camera_on;
  const bool missing_after_video =
      media.EverHadRemoteVideo() && expect_remote_video && !media.HasRemoteVideo();
  const bool p2p_failed =
      calls && calls->IsP2pConnectFailed() && !calls->IsAwaitingSfuRecovery() && !media.IsSfuMode();
  const bool media_reconnect =
      !p2p_failed && (stalling || missing_after_video || conn == "disconnected");

  // Live or soft-stall: keep painting the last frame. Hard stall / camera-off clears HasRemoteVideo.
  in_call.remote_video = media.HasRemoteVideo() && expect_remote_video;
  if (!in_call.remote_video) {
    CallVideoTileRenderer::Instance().ClearRemote();
  }

  CallMediaEngine::VideoTileFrame local_tile;
  in_call.local_preview = media.CopyLocalVideoFrame(local_tile);
  if (in_call.local_preview) {
    CallVideoTileRenderer::Frame frame;
    frame.width = local_tile.width;
    frame.height = local_tile.height;
    frame.seq = local_tile.seq;
    frame.rgba = std::move(local_tile.rgba);
    CallVideoTileRenderer::Instance().SubmitLocalFrame(std::move(frame));
  }
  if (in_call.remote_video) {
    CallMediaEngine::VideoTileFrame remote_tile;
    if (media.CopyRemoteVideoFrame(remote_tile)) {
      CallVideoTileRenderer::Frame frame;
      frame.width = remote_tile.width;
      frame.height = remote_tile.height;
      frame.seq = remote_tile.seq;
      frame.rgba = std::move(remote_tile.rgba);
      CallVideoTileRenderer::Instance().SubmitRemoteFrame(std::move(frame));
    }
  }

  in_call.stage_visible = in_call.camera_on || in_call.remote_video || in_call.local_preview ||
                          (have_peer_video_flag && peer_camera_on) || media_reconnect;
  if (in_call.remote_video) {
    in_call.remote_placeholder = "";
  } else if (have_peer_video_flag && !peer_camera_on) {
    in_call.remote_placeholder = Tr("call.placeholder.camera_off").c_str();
  } else if (p2p_failed) {
    in_call.remote_placeholder = Tr("call.status.couldnt_connect").c_str();
  } else if (media_reconnect) {
    in_call.remote_placeholder = Tr("call.status.reconnecting").c_str();
  } else {
    in_call.remote_placeholder = "";
  }

  in_call.mic_level = muted ? 0 : QuantizeAudioLevel(media.LocalInputLevel());
  in_call.peer_level = QuantizeAudioLevel(media.RemoteOutputLevel());
  in_call.mic_hint = LevelHint(in_call.mic_level, false, muted).c_str();
  in_call.peer_hint = LevelHint(in_call.peer_level, true, false).c_str();
  if (p2p_failed) {
    in_call.subtitle = Tr("call.status.couldnt_connect").c_str();
  } else if (media_reconnect && !media.IsConnected()) {
    in_call.subtitle = Tr("call.status.reconnecting").c_str();
  } else if (stalling) {
    in_call.subtitle = Tr("call.status.reconnecting").c_str();
  } else if (media.IsConnected()) {
    in_call.elapsed = FormatElapsed(media.ConnectedAtMs());
    if (!in_call.elapsed.empty()) {
      in_call.subtitle = in_call.elapsed;
    }
  }
}

void CallController::RefreshCallLevels() {
  if (active_call_id_.empty()) {
    return;
  }
  auto* calls = Hub().Calls();
  if (!calls) {
    return;
  }
  // Keep Calling… / timer / levels fresh even before media starts.
  RefreshPendingRing();
}

} // namespace pbr
