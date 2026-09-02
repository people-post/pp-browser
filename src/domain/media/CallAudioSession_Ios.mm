#include "domain/media/CallAudioSession.h"

#include <TargetConditionals.h>

#if defined(__APPLE__) && TARGET_OS_IPHONE

#import <AVFoundation/AVFoundation.h>

#include <atomic>

namespace pbr {
namespace CallAudioSession {
namespace {

/** Default earpiece; user opts into speakerphone. Reset each call on Deactivate. */
std::atomic<bool> g_speakerphone{false};
std::atomic<bool> g_session_active{false};

void ApplyRoute(bool speaker_on) {
  AVAudioSession* session = [AVAudioSession sharedInstance];
  NSError* error = nil;
  const AVAudioSessionPortOverride port =
      speaker_on ? AVAudioSessionPortOverrideSpeaker : AVAudioSessionPortOverrideNone;
  [session overrideOutputAudioPort:port error:&error];
  (void)error;
}

} // namespace

void ActivateForVoipCall() {
  AVAudioSession* session = [AVAudioSession sharedInstance];
  NSError* error = nil;
  AVAudioSessionCategoryOptions options = AVAudioSessionCategoryOptionAllowBluetooth |
                                          AVAudioSessionCategoryOptionAllowBluetoothA2DP;
  if (g_speakerphone.load()) {
    options |= AVAudioSessionCategoryOptionDefaultToSpeaker;
  }
  [session setCategory:AVAudioSessionCategoryPlayAndRecord withOptions:options error:&error];
  [session setMode:AVAudioSessionModeVoiceChat error:&error];
  [session setActive:YES error:&error];
  g_session_active.store(true);
  ApplyRoute(g_speakerphone.load());
  (void)error;
}

void Deactivate() {
  g_session_active.store(false);
  g_speakerphone.store(false);
  AVAudioSession* session = [AVAudioSession sharedInstance];
  NSError* error = nil;
  [session overrideOutputAudioPort:AVAudioSessionPortOverrideNone error:&error];
  [session setActive:NO withOptions:AVAudioSessionSetActiveOptionNotifyOthersOnDeactivation error:&error];
  (void)error;
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
  AVAudioSessionCategoryOptions options = AVAudioSessionCategoryOptionAllowBluetooth |
                                          AVAudioSessionCategoryOptionAllowBluetoothA2DP;
  if (on) {
    options |= AVAudioSessionCategoryOptionDefaultToSpeaker;
  }
  [session setCategory:AVAudioSessionCategoryPlayAndRecord withOptions:options error:&error];
  ApplyRoute(on);
  (void)error;
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
