#if defined(__ANDROID__)

#include "lib/media/AudioSession.h"

#include "lib/media/AudioFocusInterruption.h"
#include "lib/media/MediaHost.h"
#include "lib/media/MicEndpoints.h"
#include "lib/media/SpeakerRouteRules.h"

#include <atomic>
#include <mutex>
#include <optional>

#include <SDL3/SDL.h>

namespace pbr {
namespace AudioSession {
namespace {

/** The call's speaker choice: default earpiece; kept across Deactivate (a media rebuild), cleared when the call ends. */
std::atomic<bool> g_speakerphone{false};
std::atomic<bool> g_session_active{false};
/**
 * The speaker route: every MediaHost::set_speakerphone call and `g_routed` under one lock, from the engine's capture
 * thread (ActivateForVoip, under MediaEngine's g_audio_session_mu), the ringback thread, the UI thread and the main
 * thread (focus back, under g_focus_mu). Lock order: g_audio_session_mu, then g_focus_mu, then g_route_mu; nothing
 * but the host's call runs under it (the host posts focus changes to its main looper, never calls back into it
 * synchronously).
 */
std::mutex g_route_mu;
/** What the route was last set to here (RoutedSpeaker), under g_route_mu; set only when the host's call went through.
 *  The next activation routes again when it differs from the choice. */
RoutedSpeaker g_routed = RoutedSpeaker::Unknown;

/**
 * Under g_route_mu. The host routes (MediaHost::set_speakerphone: the app's activity owns AudioManager); its answer is
 * whether it did.
 */
void RouteSpeakerLocked(bool on) {
  const MediaHost host = CurrentMediaHost();
  if (host.set_speakerphone && host.set_speakerphone(on)) {
    g_routed = on ? RoutedSpeaker::On : RoutedSpeaker::Off;
  }
}

void ApplySpeakerphoneLocked(bool on) {
  std::lock_guard lock(g_route_mu);
  RouteSpeakerLocked(on);
}

void ApplyVoipModeLocked(bool active) {
  const MediaHost host = CurrentMediaHost();
  if (host.set_call_audio_mode) {
    (void)host.set_call_audio_mode(active);
  }
}

/**
 * The call's audio focus as an interruption (AudioFocusInterruption). Its lock also keeps a focus coming back (the
 * main thread) from putting the call's route back after Deactivate let it go.
 */
std::mutex g_focus_mu;
AudioFocusInterruption g_focus; // under g_focus_mu

/** Under g_focus_mu: the call's audio mode, communication device and speaker choice, which the other call changed. */
void RestoreCallRouteLocked() {
  if (!g_session_active.load()) {
    return;
  }
  ApplyVoipModeLocked(true);
  ApplySpeakerphoneLocked(g_speakerphone.load());
}

/** Under g_focus_mu: what `step` does there; true when the interruption is over (notify after the lock). */
bool TakeFocusStepLocked(const AudioFocusInterruption::Step step) {
  switch (step) {
    case AudioFocusInterruption::Step::Begin:
      SDL_Log("AudioSession: call audio focus lost — interrupted");
      NotifyInterruptionBegan();
      return false;
    case AudioFocusInterruption::Step::RestoreAndEnd:
      SDL_Log("AudioSession: call audio focus back — restoring the route, resuming");
      RestoreCallRouteLocked();
      return true;
    case AudioFocusInterruption::Step::None:
      return false;
  }
  return false;
}

/**
 * After a permanent loss of the focus (nothing gives it back): ask for it again, and a grant ends the interruption.
 * `why`: what brought the user back (the app in front, a call control).
 */
void RetakeFocusIfLost(const char* why) {
  bool ended = false;
  {
    std::lock_guard lock(g_focus_mu);
    if (!g_focus.WantsRetake(g_session_active.load())) {
      return;
    }
    const MediaHost host = CurrentMediaHost();
    const bool granted = host.retake_call_audio_focus ? host.retake_call_audio_focus().value_or(false) : false;
    SDL_Log("AudioSession: call audio focus asked again (%s): %s", why, granted ? "granted" : "not yet");
    ended = TakeFocusStepLocked(g_focus.OnRetake(granted, IsInterrupted()));
  }
  if (ended) {
    NotifyInterruptionEnded();
  }
}

/** Cached: distinct earpiece vs loudspeaker (false on most tablets). */
std::atomic<int> g_has_earpiece{-1}; // -1 unknown, 0 no, 1 yes

bool QueryHasEarpieceRoute() {
  const int cached = g_has_earpiece.load(std::memory_order_acquire);
  if (cached >= 0) {
    return cached != 0;
  }
  const MediaHost host = CurrentMediaHost();
  const std::optional<bool> has = host.has_earpiece_route ? host.has_earpiece_route() : std::nullopt;
  if (!has) {
    return true; // fail open: keep toggle until the host can tell (its activity is ready)
  }
  g_has_earpiece.store(*has ? 1 : 0, std::memory_order_release);
  return *has;
}

} // namespace

void ActivateForVoip() {
  const bool was_active = g_session_active.exchange(true);
  ApplyVoipModeLocked(true);
  // Tablets with no earpiece: only loudspeaker exists — start on speaker so we don't
  // sit on FORCE_NONE with the same device and a quieter comms gain curve.
  if (!was_active && !QueryHasEarpieceRoute()) {
    g_speakerphone.store(true);
  }
  // Only when entering a call session, or when the route is not the choice (a call that ended forgot its choice
  // without routing): re-applying it on every reopen races OEM route changes (Moto) and can kill AAudio capture.
  std::lock_guard lock(g_route_mu);
  if (PushSpeakerRouteOnActivate(was_active, g_routed, g_speakerphone.load())) {
    RouteSpeakerLocked(g_speakerphone.load());
  }
}

void Deactivate() {
  std::lock_guard lock(g_focus_mu);
  g_session_active.store(false);
  g_focus.Reset();
  ClearInterruption();
  ApplySpeakerphoneLocked(false);
  ApplyVoipModeLocked(false);
}

void CancelPendingDeactivate() {}

void SetSystemManaged(bool /*managed*/) {}
void NotifySystemActivation(bool /*active*/) {}

// Voice messages: SDL opens playback with the media role (speaker, media volume) and capture needs no switch.
void ActivateForVoicePlayback() {}
void ActivateForVoiceRecording() {}
void ReleaseVoice() {}

bool SupportsSpeakerToggle() {
  return QueryHasEarpieceRoute();
}

bool IsSpeakerphoneOn() {
  return g_speakerphone.load();
}

void SetSpeakerphoneOn(bool on) {
  g_speakerphone.store(on);
  if (g_session_active.load()) {
    RetakeFocusIfLost("speaker"); // the user is back at the call: a focus lost for good is asked for again
    ApplySpeakerphoneLocked(on);
  }
}

void ForgetSpeakerChoice() {
  g_speakerphone.store(false);
  // Back to the earpiece now only when no mic is open (RouteBackWhenChoiceForgotten). With the call's mic still open
  // (its engine closes it on the retire thread) Deactivate routes back after that close, or the next activation does.
  std::lock_guard lock(g_route_mu);
  if (RouteBackWhenChoiceForgotten(g_session_active.load(), AnyMicEndpointOpen())) {
    RouteSpeakerLocked(false);
  }
}

bool SpeakerToggleNeedsDeviceReopen() {
  return true;
}

void NotifyVoiceProcessingStarted(double /*sample_rate*/, int /*channels*/, int /*bypass*/, int /*agc*/) {}
bool MicNotOpenHintApplies() {
  return true;
}

void NoteToneStreamClosed(const char* /*holder*/, int64_t /*since_stop_ms*/) {}

int OpusEncoderComplexity() {
  return 5;
}

int CaptureOpenAttemptCount() {
  return 4;
}

int CaptureOpenRetryDelayMs(int attempt_index) {
  constexpr int kRetryDelayMs[] = {0, 80, 200, 400};
  if (attempt_index < 0 || attempt_index >= static_cast<int>(sizeof(kRetryDelayMs) / sizeof(kRetryDelayMs[0]))) {
    return 0;
  }
  return kRetryDelayMs[attempt_index];
}

int CaptureReopenSettleDelayMs() {
  return 150;
}

void ApplyCaptureAudioHints() {
  // Stock SDL 3.4.16 (B35): "Communications" role → AAUDIO_USAGE_VOICE_COMMUNICATION; input
  // preset 7 = VOICE_COMMUNICATION engages the OEM AEC/NS. Speaker-only tablets keep MEDIA
  // usage (paired with MODE_NORMAL in the host's call mode; IN_COMMUNICATION+MEDIA is whisper-quiet).
  if (SupportsSpeakerToggle()) {
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_STREAM_ROLE, "Communications");
    SDL_SetHint(SDL_HINT_ANDROID_AAUDIO_INPUT_PRESET, "7");
  } else {
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_STREAM_ROLE, "");
    SDL_SetHint(SDL_HINT_ANDROID_AAUDIO_INPUT_PRESET, "");
  }
}

void ClearCaptureAudioHints() {
  SDL_SetHint(SDL_HINT_AUDIO_DEVICE_STREAM_ROLE, "");
  SDL_SetHint(SDL_HINT_ANDROID_AAUDIO_INPUT_PRESET, "");
}

void NotifyHostCallAudioFocus(const int focus_change) {
  bool ended = false;
  {
    std::lock_guard lock(g_focus_mu);
    const AudioFocusInterruption::Step step =
        g_focus.OnChange(AudioFocusInterruption::FromAndroid(focus_change), g_session_active.load(), IsInterrupted());
    ended = TakeFocusStepLocked(step);
  }
  if (ended) {
    NotifyInterruptionEnded();
  }
}

void NotifyHostResumed() {
  RetakeFocusIfLost("app in front");
}

} // namespace AudioSession
} // namespace pbr

#endif // defined(__ANDROID__)
