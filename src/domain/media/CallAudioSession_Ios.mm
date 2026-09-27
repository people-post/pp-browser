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
 * Loudspeaker uses the default mode: on device (2026-09-27) both voice modes (VoiceChat and
 * VideoChat) left the loudspeaker very quiet at max volume, while SDL's former ModeDefault was
 * loud. The earpiece keeps VoiceChat (call-volume scale, earpiece tuning).
 */
NSString* ModeForRoute(bool speaker_on) {
  return speaker_on ? AVAudioSessionModeDefault : AVAudioSessionModeVoiceChat;
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
  [session setActive:YES error:&error];
  g_session_active.store(true);
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
