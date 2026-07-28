#include "feature/ui/CallController.h"

#include "base/media/CallMediaEngine.h"
#include "base/messaging/CallTypes.h"
#include "base/people/ContactTypes.h"
#include "base/platform/ILocalNotifier.h"
#include "base/ui/ShellTypes.h"
#include "feature/messaging/MessagingHub.h"
#include "feature/ui/CallChromeSync.h"
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
      .ring_call_id = state.call_ring.call_id.c_str(),
      .in_call_id = state.call_in_progress.call_id.c_str(),
      .in_call_subtitle = state.call_in_progress.subtitle.c_str(),
      .ring_caller_label = state.call_ring.caller_label.c_str(),
      .ring_media_label = state.call_ring.media_label.c_str(),
      .in_call_title = state.call_in_progress.title.c_str(),
      .in_call_mic_level = state.call_in_progress.mic_level,
      .in_call_peer_level = state.call_in_progress.peer_level,
      .in_call_mic_hint = state.call_in_progress.mic_hint.c_str(),
      .in_call_peer_hint = state.call_in_progress.peer_hint.c_str(),
      .in_call_elapsed = state.call_in_progress.elapsed.c_str(),
      .in_call_peer_label = state.call_in_progress.peer_label.c_str(),
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

void CallController::BindToMessaging() {
  if (bound_ || !MessagingHub::Instance().IsInitialized()) {
    return;
  }
  if (auto* calls = MessagingHub::Instance().Calls()) {
    calls->SetOnRingChanged([this]() { RefreshPendingRing(); });
    bound_ = true;
  }
}

void CallController::Tick() {
  BindToMessaging();
  if (auto* calls = MessagingHub::Instance().Calls()) {
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
  RefreshPendingRing();
  if (!ringing_call_id_.empty()) {
    const auto& label = ShellHost::Instance().State().call_ring.caller_label;
    const std::string body = label.empty() ? "Someone is calling you" : (std::string(label.c_str()) + " is calling");
    ILocalNotifier::Instance().NotifyIncoming("Incoming call", body, "");
  }
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
  if (auto contact = MessagingHub::Instance().Contacts().FindByIdentity(identity, ContactIdKind::RelayUser)) {
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
  auto* calls = MessagingHub::Instance().Calls();
  if (!calls) {
    return;
  }

  if (auto media_err = calls->TakeLastMediaError(); media_err && !media_err->empty()) {
    UserFeedback::Fail(*media_err);
  }

  auto top = calls->TopPendingInvite();
  if (top && top->has_value()) {
    ClearInCall();
    ringing_call_id_ = (*top)->call_id;
    if (ring_started_ms_ == 0) {
      ring_started_ms_ = util::NowUnixMs();
    }
    auto& ring = ShellHost::Instance().State().call_ring;
    ring.active = true;
    ring.call_id = (*top)->call_id;
    ring.caller_label = DisplayNameForIdentity((*top)->inviter_identity);
    if (ring.caller_label.empty()) {
      ring.caller_label = (*top)->inviter_identity;
    }
    ring.media_label =
        (*top)->media_mode == CallMediaMode::Video ? "Incoming video call" : "Incoming voice call";
    SyncShellState();
    SyncRingtone();
    return;
  }

  if (auto active = calls->ActiveLocalCall(); active && active->has_value()) {
    active_call_id_ = (*active)->call_id;
    ClearRing();
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
  auto* calls = MessagingHub::Instance().Calls();
  if (!calls) {
    UserFeedback::Fail("Calls unavailable");
    return false;
  }
  auto thread = MessagingHub::Instance().Store().GetThread(thread_id);
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
  auto* calls = MessagingHub::Instance().Calls();
  if (!calls || ringing_call_id_.empty()) {
    return;
  }
  ringtone_.Stop();
  if (auto accepted = calls->AcceptInvite(ringing_call_id_); !accepted) {
    UserFeedback::Fail(accepted.error().message);
  }
  RefreshPendingRing();
}

void CallController::DeclineIncoming() {
  BindToMessaging();
  auto* calls = MessagingHub::Instance().Calls();
  if (!calls || ringing_call_id_.empty()) {
    return;
  }
  ringtone_.Stop();
  (void)calls->DeclineInvite(ringing_call_id_);
  RefreshPendingRing();
}

void CallController::LeaveActive() {
  BindToMessaging();
  auto* calls = MessagingHub::Instance().Calls();
  if (!calls || active_call_id_.empty()) {
    return;
  }
  (void)calls->LeaveCall(active_call_id_);
  RefreshPendingRing();
}

void CallController::ToggleMute() {
  BindToMessaging();
  auto* calls = MessagingHub::Instance().Calls();
  if (!calls || !calls->Media().IsActive()) {
    return;
  }
  calls->Media().SetMuted(!calls->Media().IsMuted());
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

void CallController::ApplyAudioLevels(CallMediaEngine& media) {
  auto& in_call = ShellHost::Instance().State().call_in_progress;
  const bool muted = media.IsMuted();
  in_call.muted = muted;
  in_call.mic_level = muted ? 0 : QuantizeAudioLevel(media.LocalInputLevel());
  in_call.peer_level = QuantizeAudioLevel(media.RemoteOutputLevel());
  in_call.mic_hint = LevelHint(in_call.mic_level, false, muted);
  in_call.peer_hint = LevelHint(in_call.peer_level, true, false);
  if (media.IsConnected()) {
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
  auto* calls = MessagingHub::Instance().Calls();
  if (!calls) {
    return;
  }
  // Keep Calling… / timer / levels fresh even before media starts.
  RefreshPendingRing();
}

} // namespace pbr
