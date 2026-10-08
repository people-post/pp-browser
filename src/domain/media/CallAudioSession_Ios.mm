#include "domain/media/CallAudioSession.h"

#include <TargetConditionals.h>

#if defined(__APPLE__) && TARGET_OS_IPHONE

#import <AVFoundation/AVFoundation.h>

#include <atomic>
#include <mutex>

// Xcode 26+ renamed AllowBluetooth → AllowBluetoothHFP; keep building on SDK 18.
#if !defined(AVAudioSessionCategoryOptionAllowBluetoothHFP)
#define AVAudioSessionCategoryOptionAllowBluetoothHFP AVAudioSessionCategoryOptionAllowBluetooth
#endif

namespace pbr {
namespace CallAudioSession {
namespace {

/** Default earpiece; user opts into speakerphone. Reset each call on Deactivate. */
std::atomic<bool> g_speakerphone{false};
std::atomic<bool> g_session_active{false};
/** Bumped by every ActivateForVoipCall; a delayed Deactivate retry gives up once it changed. */
std::atomic<uint64_t> g_session_gen{0};
/** Makes "still ours?" + setActive:NO atomic against a new activation / CancelPendingDeactivate. */
std::mutex g_session_mu;
constexpr int kDeactivateRetries = 5;
constexpr int64_t kDeactivateRetryNs = 150 * NSEC_PER_MSEC;

/** Diagnostic: what iOS actually ended up with (category / mode / options / output port). */
void LogRoute(const char* where) {
  AVAudioSession* session = [AVAudioSession sharedInstance];
  NSString* out = @"-";
  if (session.currentRoute.outputs.count > 0) {
    out = session.currentRoute.outputs.firstObject.portType;
  }
  NSLog(@"CallAudioSession %s: speaker_flag=%d category=%@ mode=%@ options=0x%lx output=%@", where,
        g_speakerphone.load() ? 1 : 0, session.category, session.mode,
        static_cast<unsigned long>(session.categoryOptions), out);
}

void ApplyRoute(bool speaker_on) {
  AVAudioSession* session = [AVAudioSession sharedInstance];
  NSError* error = nil;
  const AVAudioSessionPortOverride port =
      speaker_on ? AVAudioSessionPortOverrideSpeaker : AVAudioSessionPortOverrideNone;
  [session overrideOutputAudioPort:port error:&error];
  (void)error;
}

/**
 * VoiceChat on both routes: the voice-processing unit (echo cancellation) is only tuned for
 * VoiceChat/VideoChat. The loudspeaker is still reached via DefaultToSpeaker + port override; its
 * volume is made up by the engine's gain on the voice-processing path.
 */
NSString* ModeForRoute(bool /*speaker_on*/) {
  return AVAudioSessionModeVoiceChat;
}

/**
 * Device closes are queued on the media device thread, so setActive:NO can land while the
 * voice-processing unit is still running and fail with IsBusy. Retry a few times, unless a newer
 * call (or ringback) activated the session meanwhile — then that one owns it.
 */
void DeactivateAttempt(uint64_t gen, int attempts_left) {
  NSError* error = nil;
  {
    std::lock_guard<std::mutex> lock(g_session_mu);
    if (g_session_gen.load() != gen || g_session_active.load()) {
      return;
    }
    if ([[AVAudioSession sharedInstance] setActive:NO
                                        withOptions:AVAudioSessionSetActiveOptionNotifyOthersOnDeactivation
                                              error:&error]) {
      return;
    }
  }
  if (error.code == AVAudioSessionErrorCodeIsBusy && attempts_left > 0) {
    NSLog(@"CallAudioSession Deactivate: IsBusy, retrying (%d left)", attempts_left);
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, kDeactivateRetryNs),
                   dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0),
                   ^{ DeactivateAttempt(gen, attempts_left - 1); });
    return;
  }
  NSLog(@"CallAudioSession Deactivate: setActive:NO failed: %@", error);
}

} // namespace

void ActivateForVoipCall() {
  AVAudioSession* session = [AVAudioSession sharedInstance];
  NSError* error = nil;
  AVAudioSessionCategoryOptions options = AVAudioSessionCategoryOptionAllowBluetoothHFP |
                                          AVAudioSessionCategoryOptionAllowBluetoothA2DP;
  if (g_speakerphone.load()) {
    options |= AVAudioSessionCategoryOptionDefaultToSpeaker;
  }
  [session setCategory:AVAudioSessionCategoryPlayAndRecord withOptions:options error:&error];
  [session setMode:ModeForRoute(g_speakerphone.load()) error:&error];
  // 48 kHz / 20 ms like the engine: fewer resampling steps and smaller voice-processing chunks.
  [session setPreferredSampleRate:48000 error:&error];
  [session setPreferredIOBufferDuration:0.02 error:&error];
  {
    std::lock_guard<std::mutex> lock(g_session_mu);
    g_session_gen.fetch_add(1);
    [session setActive:YES error:&error];
    g_session_active.store(true);
  }
  ApplyRoute(g_speakerphone.load());
  LogRoute("ActivateForVoipCall");
  (void)error;
}

void Deactivate() {
  g_session_active.store(false);
  g_speakerphone.store(false);
  AVAudioSession* session = [AVAudioSession sharedInstance];
  NSError* error = nil;
  [session overrideOutputAudioPort:AVAudioSessionPortOverrideNone error:&error];
  (void)error;
  DeactivateAttempt(g_session_gen.load(), kDeactivateRetries);
}

void CancelPendingDeactivate() {
  std::lock_guard<std::mutex> lock(g_session_mu);
  g_session_gen.fetch_add(1);
}

bool SupportsSpeakerToggle() {
  return true;
}

bool IsSpeakerphoneOn() {
  return g_speakerphone.load();
}

void SetSpeakerphoneOn(bool on) {
  g_speakerphone.store(on);
  if (!g_session_active.load()) {
    return;
  }
  AVAudioSession* session = [AVAudioSession sharedInstance];
  NSError* error = nil;
  AVAudioSessionCategoryOptions options = AVAudioSessionCategoryOptionAllowBluetoothHFP |
                                          AVAudioSessionCategoryOptionAllowBluetoothA2DP;
  if (on) {
    options |= AVAudioSessionCategoryOptionDefaultToSpeaker;
  }
  [session setCategory:AVAudioSessionCategoryPlayAndRecord withOptions:options error:&error];
  [session setMode:ModeForRoute(on) error:&error];
  ApplyRoute(on);
  LogRoute("SetSpeakerphoneOn");
  (void)error;
}

bool SpeakerToggleNeedsDeviceReopen() {
  return false;
}

int OpusEncoderComplexity() {
  return 5;
}

int CaptureOpenAttemptCount() {
  return 1;
}

int CaptureOpenRetryDelayMs(int /*attempt_index*/) {
  return 0;
}

int CaptureReopenSettleDelayMs() {
  return 0;
}

void ApplyCaptureAudioHints() {}
void ClearCaptureAudioHints() {}

} // namespace CallAudioSession
} // namespace pbr

#endif
