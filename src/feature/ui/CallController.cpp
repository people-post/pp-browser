#include <stdexcept>
#include "feature/ui/CallController.h"

#include "base/media/CallMediaEngine.h"
#include "base/messaging/CallTypes.h"
#include "base/people/ContactTypes.h"
#include "base/platform/BrowserThread.h"
#include "base/platform/ILocalNotifier.h"
#include "base/ui/ShellTypes.h"
#include "feature/messaging/MessagingHub.h"
#include "feature/ui/CallChromeSync.h"
#include "feature/ui/CallConflictCopy.h"
#include "CallVideoTileRenderer.h"
#include "feature/ui/ShellHost.h"
#include "feature/ui/UserFeedback.h"

#include "common/Utilities.h"

#include <algorithm>
#include <cstdio>

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

const char* LevelHint(int level, bool remote, bool muted) {
  if (muted && !remote) {
    return "Muted";
  }
  if (level <= 0) {
    return remote ? "Quiet" : "Silent";
  }
  if (level <= 2) {
    return "Speaking";
  }
  return "Loud";
}

} // namespace

CallController& CallController::Instance() {
  static CallController instance;
  return instance;
}
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
  if (!messaging_ || !Instance().Hub().IsInitialized()) {
    bound_calls_ = nullptr;
    return;
  }
  auto* calls = Instance().Hub().Calls();
  if (!calls) {
    bound_calls_ = nullptr;
    return;
  }
  // CallSessionManager is recreated in BuildMessagingStack; rebind when the pointer changes.
  if (bound_calls_ == calls) {
    return;
  }
  calls->SetOnRingChanged([]() {
    // Ingest may run on IO; shell/RmlUi updates must stay on UI.
    BrowserThread::PostTask(BrowserThreadId::UI, []() { CallController::Instance().RefreshPendingRing(); });
  });
  bound_calls_ = calls;
}

void CallController::Tick() {
  BindToMessaging();
  if (auto* calls = Instance().Hub().Calls()) {
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
  if (identity.empty()) {
    return {};
  }
  if (auto contact = Instance().Hub().Contacts().FindByIdentity(identity, ContactIdKind::RelayUser)) {
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
  auto* calls = Instance().Hub().Calls();
  if (!calls) {
    return;
  }

  if (auto media_err = calls->TakeLastMediaError(); media_err && !media_err->empty()) {
    UserFeedback::Fail(*media_err);
  }

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
    ring.media_label =
        (*top)->media_mode == CallMediaMode::Video ? "Incoming video call" : "Incoming voice call";
    ring.eyebrow = copy.eyebrow;
    ring.conflict_hint = copy.hint;
    ring.accept_label = copy.accept_label;
    ring.decline_label = copy.decline_label;
    if (pending_call_wake_notify_) {
      pending_call_wake_notify_ = false;
      const std::string body =
          caller_label.empty() ? "Someone is calling you" : (caller_label + " is calling");
      ILocalNotifier::Instance().NotifyIncoming("Incoming call", body, "");
    }
    SyncShellState();
    SyncRingtone();
    return;
  }

  if (auto active = calls->ActiveLocalCall(); active && active->has_value()) {
    active_call_id_ = (*active)->call_id;
    ClearRing();

    // Peer vanished without a clean leave — end local session so chrome does not stick.
    if (calls->Media().IsActive() && calls->Media().ActiveCallId() == active_call_id_) {
      const std::string media_state = calls->Media().ConnectionState();
      if (media_state == "failed") {
        (void)calls->LeaveCall(active_call_id_);
        ClearInCall();
        ClearRing();
        SyncShellState();
        return;
      }
    }

    auto& in_call = ShellHost::Instance().State().call_in_progress;
    in_call.active = true;
    in_call.call_id = (*active)->call_id;
    in_call.title = (*active)->media_mode == CallMediaMode::Video ? "Video call" : "Voice call";
    in_call.muted = calls->Media().IsMuted();

    std::string peer_label = "Them";
    if (auto peer = calls->PeerIdentityForCall((*active)->call_id); peer && peer->has_value()) {
      const std::string name = DisplayNameForIdentity(**peer);
      if (!name.empty()) {
        peer_label = name;
      }
    }
    in_call.peer_label = peer_label;

    if (calls->Media().IsConnected()) {
      in_call.elapsed = FormatElapsed(calls->Media().ConnectedAtMs());
      in_call.subtitle = in_call.elapsed.empty() ? "Connected" : in_call.elapsed;
    } else if (!calls->Media().IsActive()) {
      in_call.elapsed = {};
      in_call.subtitle = "Calling…";
    } else {
      in_call.elapsed = {};
      const std::string state = calls->Media().ConnectionState();
      if (state == "connecting" || state.empty()) {
        in_call.subtitle = "Connecting…";
      } else if (state == "disconnected") {
        in_call.subtitle = "Reconnecting…";
      } else {
        in_call.subtitle = state;
      }
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
  auto* calls = Instance().Hub().Calls();
  if (!calls) {
    UserFeedback::Fail("Calls unavailable");
    return false;
  }
  auto thread = Instance().Hub().Store().GetThread(thread_id);
  if (!thread || !*thread || (*thread)->kind != ThreadKind::Direct) {
    UserFeedback::Fail("Voice and video calls are available in 1:1 chats");
    return false;
  }
  if ((*thread)->peer_identity_value.empty()) {
    UserFeedback::Fail("Missing peer identity");
    return false;
  }
  auto started =
      calls->StartCall(thread_id, video ? CallMediaMode::Video : CallMediaMode::Voice, {(*thread)->peer_identity_value});
  if (!started) {
    UserFeedback::Fail(started.error().message);
    return false;
  }
  active_call_id_ = started->call_id;
  RefreshPendingRing();
  return true;
}

bool CallController::StartVoiceCall(const std::string& thread_id) {
  return StartCall(thread_id, false);
}

bool CallController::StartVideoCall(const std::string& thread_id) {
  return StartCall(thread_id, true);
}

void CallController::AcceptIncoming() {
  BindToMessaging();
  auto* calls = Instance().Hub().Calls();
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
  if (auto accepted = calls->AcceptInvite(ringing_call_id_); !accepted) {
    UserFeedback::Fail(accepted.error().message);
  }
  RefreshPendingRing();
}

void CallController::DeclineIncoming() {
  BindToMessaging();
  auto* calls = Instance().Hub().Calls();
  if (!calls || ringing_call_id_.empty()) {
    return;
  }
  ringtone_.Stop();
  (void)calls->DeclineInvite(ringing_call_id_);
  RefreshPendingRing();
}

void CallController::LeaveActive() {
  BindToMessaging();
  auto* calls = Instance().Hub().Calls();
  if (!calls || active_call_id_.empty()) {
    return;
  }
  (void)calls->LeaveCall(active_call_id_);
  RefreshPendingRing();
}

void CallController::ToggleMute() {
  BindToMessaging();
  auto* calls = Instance().Hub().Calls();
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
  auto* calls = Instance().Hub().Calls();
  if (!calls || !calls->Media().IsActive()) {
    return;
  }
  const bool next = !calls->Media().IsCameraEnabled();
  if (auto cam = calls->SetLocalVideoEnabled(next); !cam) {
    UserFeedback::Fail(cam.error().message);
  }
  RefreshPendingRing();
}

void CallController::AcceptCallback(Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
  Instance().AcceptIncoming();
}

void CallController::DeclineCallback(Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
  Instance().DeclineIncoming();
}

void CallController::LeaveCallback(Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
  Instance().LeaveActive();
}

void CallController::MuteCallback(Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
  Instance().ToggleMute();
}

void CallController::CameraCallback(Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
  Instance().ToggleCamera();
}

void CallController::ApplyAudioLevels(CallMediaEngine& media) {
  media.RefreshRemoteVideoHealth();

  auto& in_call = ShellHost::Instance().State().call_in_progress;
  const bool muted = media.IsMuted();
  in_call.muted = muted;
  in_call.camera_on = media.IsCameraEnabled();

  bool peer_camera_on = false;
  bool have_peer_video_flag = false;
  auto* calls = Instance().Hub().Calls();
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
  const bool media_reconnect =
      stalling || missing_after_video || conn == "disconnected" || conn == "failed";

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
    in_call.remote_placeholder = "Camera off";
  } else if (media_reconnect) {
    in_call.remote_placeholder = "Reconnecting…";
  } else {
    in_call.remote_placeholder = "";
  }

  in_call.mic_level = muted ? 0 : QuantizeAudioLevel(media.LocalInputLevel());
  in_call.peer_level = QuantizeAudioLevel(media.RemoteOutputLevel());
  in_call.mic_hint = LevelHint(in_call.mic_level, false, muted);
  in_call.peer_hint = LevelHint(in_call.peer_level, true, false);
  if (media_reconnect && !media.IsConnected()) {
    in_call.subtitle = "Reconnecting…";
  } else if (stalling) {
    in_call.subtitle = "Reconnecting…";
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
  auto* calls = Instance().Hub().Calls();
  if (!calls) {
    return;
  }
  // Keep Calling… / timer / levels fresh even before media starts.
  RefreshPendingRing();
}

} // namespace pbr
