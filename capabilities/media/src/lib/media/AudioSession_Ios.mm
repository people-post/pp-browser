#include "lib/media/AudioSession.h"

#include <TargetConditionals.h>

#if defined(__APPLE__) && TARGET_OS_IPHONE

#import <AVFoundation/AVFoundation.h>
#import <UIKit/UIKit.h>

#include "common/Logger.h"
#include "common/Metrics.h"

#include <atomic>
#include <mutex>

// Enum cases are not preprocessor macros — do not use #if !defined(...).
// SDK 26+: AllowBluetoothHFP; older SDKs: AllowBluetooth (same bit).
#if !defined(__IPHONE_26_0) || (__IPHONE_OS_VERSION_MAX_ALLOWED < __IPHONE_26_0)
#define AVAudioSessionCategoryOptionAllowBluetoothHFP AVAudioSessionCategoryOptionAllowBluetooth
#endif

namespace pbr {
namespace AudioSession {
namespace {

/** The call's speaker choice: default earpiece; kept across Deactivate (a media rebuild), cleared when the call ends. */
std::atomic<bool> g_speakerphone{false};
std::atomic<bool> g_session_active{false};
/** Bumped by every ActivateForVoip; a delayed Deactivate retry gives up once it changed. */
std::atomic<uint64_t> g_session_gen{0};
/** Makes "still ours?" + setActive:NO atomic against a new activation / CancelPendingDeactivate. */
std::mutex g_session_mu;
constexpr int kDeactivateRetries = 5;
constexpr int64_t kDeactivateRetryNs = 150 * NSEC_PER_MSEC;

void ResumeAfterInterruption(const char* why);
/** The app came back with no interruption end seen: resume only if the session can be had (a phone call may still hold it). */
void ResumeIfSessionFree();

/** The app's file log (NSLog does not reach it). */
logging::Logger& SessionLog() {
  static logging::Logger log = logging::getLogger("AudioSession");
  return log;
}

const char* Utf8(NSString* text) {
  return text ? text.UTF8String : "-";
}

/** AVAudioSessionInterruptionReasonKey; raw values: 1 and 4 are deprecated / newer than the deployment target. */
ShareWord InterruptionReasonWord(NSUInteger reason) {
  switch (reason) {
    case 0: return "default";
    case 1: return "app_was_suspended";
    case 2: return "built_in_mic_muted";
    case 4: return "route_disconnected";  // iOS 17+: no "ended" follows
    default: return "other";
  }
}

ShareWord RouteChangeReasonWord(AVAudioSessionRouteChangeReason reason) {
  switch (reason) {
    case AVAudioSessionRouteChangeReasonNewDeviceAvailable: return "NewDeviceAvailable";
    case AVAudioSessionRouteChangeReasonOldDeviceUnavailable: return "OldDeviceUnavailable";
    case AVAudioSessionRouteChangeReasonCategoryChange: return "CategoryChange";
    case AVAudioSessionRouteChangeReasonOverride: return "Override";
    case AVAudioSessionRouteChangeReasonWakeFromSleep: return "WakeFromSleep";
    case AVAudioSessionRouteChangeReasonNoSuitableRouteForCategory: return "NoSuitableRouteForCategory";
    case AVAudioSessionRouteChangeReasonRouteConfigurationChange: return "RouteConfigurationChange";
    default: return "Unknown";
  }
}

/** The current output's port type (never its name: that can be a person's "AirPods of …"). */
ShareWord OutputPortWord() {
  AVAudioSessionRouteDescription* route = [AVAudioSession sharedInstance].currentRoute;
  NSString* port = route.outputs.count > 0 ? route.outputs.firstObject.portType : nil;
  if (port == nil) return "none";
  if ([port isEqualToString:AVAudioSessionPortBuiltInReceiver]) return "Receiver";
  if ([port isEqualToString:AVAudioSessionPortBuiltInSpeaker]) return "Speaker";
  if ([port isEqualToString:AVAudioSessionPortHeadphones]) return "Headphones";
  if ([port isEqualToString:AVAudioSessionPortBluetoothHFP]) return "BluetoothHFP";
  if ([port isEqualToString:AVAudioSessionPortBluetoothA2DP]) return "BluetoothA2DPOutput";
  if ([port isEqualToString:AVAudioSessionPortBluetoothLE]) return "BluetoothLE";
  if ([port isEqualToString:AVAudioSessionPortUSBAudio]) return "USBAudio";
  if ([port isEqualToString:AVAudioSessionPortLineOut]) return "LineOut";
  if ([port isEqualToString:AVAudioSessionPortHDMI]) return "HDMI";
  if ([port isEqualToString:AVAudioSessionPortAirPlay]) return "AirPlay";
  if ([port isEqualToString:AVAudioSessionPortCarAudio]) return "CarAudio";
  return "other";
}

/**
 * The device the session was using went away while the OS had the session: iOS sends no "ended" for that
 * (route_disconnected), and the app stays in the foreground, so nothing else resumes — retry shortly, and once more.
 * A resume only happens when the session can be had (a phone call keeps it), tried with setActive. One serial queue:
 * the second try sees the first's resume.
 */
void ResumeSoonIfSessionFree() {
  static dispatch_queue_t queue = dispatch_queue_create("pp.audio.resume", DISPATCH_QUEUE_SERIAL);
  for (const int64_t ms : {300, 1000}) {
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, ms * NSEC_PER_MSEC), queue, ^{
      ResumeIfSessionFree();
    });
  }
}

/**
 * Once, at the first live session: an interruption that begins marks the session interrupted (NotifyInterruptionBegan);
 * its end, a media-services reset, or the app becoming active again with no end seen (iOS does not always send one)
 * resume the session and run every AudioSession interruption handler (AudioSessionInterruptions.cpp).
 */
void InstallInterruptionObservers() {
  static dispatch_once_t once;
  dispatch_once(&once, ^{
    NSNotificationCenter* center = [NSNotificationCenter defaultCenter];
    [center addObserverForName:AVAudioSessionInterruptionNotification
                        object:nil
                         queue:nil
                    usingBlock:^(NSNotification* note) {
                      const auto type = static_cast<AVAudioSessionInterruptionType>(
                          [note.userInfo[AVAudioSessionInterruptionTypeKey] unsignedIntegerValue]);
                      if (type == AVAudioSessionInterruptionTypeEnded) {
                        dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0),
                                       ^{ ResumeAfterInterruption("interruption ended"); });
                      } else if (type == AVAudioSessionInterruptionTypeBegan && g_session_active.load()) {
                        NSNumber* reason_value = note.userInfo[AVAudioSessionInterruptionReasonKey];
                        const ShareWord reason =
                            reason_value ? InterruptionReasonWord(reason_value.unsignedIntegerValue) : ShareWord("none");
                        SessionLog().info << "AudioSession interruption began reason=" << reason.view();
                        NotifyInterruptionBegan();
                        if (reason_value && reason_value.unsignedIntegerValue == 4) {  // route_disconnected
                          ResumeSoonIfSessionFree();
                        }
                      }
                    }];
    [center addObserverForName:AVAudioSessionRouteChangeNotification
                        object:nil
                         queue:nil
                    usingBlock:^(NSNotification* note) {
                      if (!g_session_active.load()) {
                        return;  // a call's route only (B178)
                      }
                      const auto reason = static_cast<AVAudioSessionRouteChangeReason>(
                          [note.userInfo[AVAudioSessionRouteChangeReasonKey] unsignedIntegerValue]);
                      const ShareWord reason_word = RouteChangeReasonWord(reason);
                      const ShareWord output = OutputPortWord();
                      SessionLog().info << "AudioSession route change reason=" << reason_word.view()
                                        << " output=" << output.view() << " interrupted=" << (IsInterrupted() ? 1 : 0);
                      MetricsLine("audio.route").Add("reason", reason_word).Add("output", output).Emit();
                      if (reason == AVAudioSessionRouteChangeReasonOldDeviceUnavailable && IsInterrupted()) {
                        ResumeSoonIfSessionFree();
                      }
                    }];
    [center addObserverForName:UIApplicationDidBecomeActiveNotification
                        object:nil
                         queue:nil
                    usingBlock:^(NSNotification*) {
                      if (IsInterrupted()) {
                        dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0),
                                       ^{ ResumeIfSessionFree(); });
                      }
                    }];
    [center addObserverForName:AVAudioSessionMediaServicesWereResetNotification
                        object:nil
                         queue:nil
                    usingBlock:^(NSNotification*) {
                      dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0),
                                     ^{ ResumeAfterInterruption("media services reset"); });
                    }];
  });
}

/** Diagnostic: what iOS actually ended up with (category / mode / options / output port). */
void LogRoute(const char* where) {
  AVAudioSession* session = [AVAudioSession sharedInstance];
  NSString* out = @"-";
  if (session.currentRoute.outputs.count > 0) {
    out = session.currentRoute.outputs.firstObject.portType;
  }
  SessionLog().info << "AudioSession " << where << ": speaker_flag=" << (g_speakerphone.load() ? 1 : 0)
                    << " category=" << Utf8(session.category) << " mode=" << Utf8(session.mode)
                    << " options=" << static_cast<unsigned long>(session.categoryOptions) << " output=" << Utf8(out);
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
    NSLog(@"AudioSession Deactivate: IsBusy, retrying (%d left)", attempts_left);
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, kDeactivateRetryNs),
                   dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
                     DeactivateAttempt(gen, attempts_left - 1);
                   });
    return;
  }
  NSLog(@"AudioSession Deactivate: setActive:NO failed: %@", error);
}

} // namespace

void ActivateForVoip() {
  InstallInterruptionObservers();
  AVAudioSession* session = [AVAudioSession sharedInstance];
  NSError* error = nil;
  AVAudioSessionCategoryOptions options =
      AVAudioSessionCategoryOptionAllowBluetoothHFP | AVAudioSessionCategoryOptionAllowBluetoothA2DP;
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
  LogRoute("ActivateForVoip");
  (void)error;
}

void Deactivate() {
  g_session_active.store(false);
  ClearInterruption();
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

namespace {

/** Re-takes the live session after the OS interrupted it (it also stops the audio units), then lets media reopen. */
void ResumeAfterInterruption(const char* why) {
  if (!g_session_active.load()) {
    return;
  }
  SessionLog().info << "AudioSession " << why << ": resuming";
  ActivateForVoip();
  NotifyInterruptionEnded();
}

void ResumeIfSessionFree() {
  if (!g_session_active.load() || !IsInterrupted()) {
    return;
  }
  NSError* error = nil;
  if (![[AVAudioSession sharedInstance] setActive:YES error:&error]) {
    SessionLog().info << "AudioSession resume: session still taken (" << Utf8(error.localizedDescription)
                      << "), staying interrupted";
    return;
  }
  ResumeAfterInterruption("no interruption end seen");
}

} // namespace

namespace {

std::atomic<bool> g_voice_active{false};

/** Voice message audio. Not while a call (or ringback) holds the session; cancels a call's pending delayed Deactivate. */
void ActivateVoice(AVAudioSessionCategory category, AVAudioSessionCategoryOptions options) {
  AVAudioSession* session = [AVAudioSession sharedInstance];
  NSError* error = nil;
  std::lock_guard<std::mutex> lock(g_session_mu);
  if (g_session_active.load()) {
    return;
  }
  g_session_gen.fetch_add(1);
  if (![session setCategory:category mode:AVAudioSessionModeDefault options:options error:&error]) {
    NSLog(@"AudioSession voice: setCategory failed: %@", error);
    error = nil;
  }
  if (![session setActive:YES error:&error]) {
    NSLog(@"AudioSession voice: setActive failed: %@", error);
  }
  g_voice_active.store(true);
}

} // namespace

// `playback` ignores the ring/silent switch and uses the loudspeaker when nothing else is plugged in.
void ActivateForVoicePlayback() {
  ActivateVoice(AVAudioSessionCategoryPlayback, 0);
}

void ActivateForVoiceRecording() {
  ActivateVoice(AVAudioSessionCategoryPlayAndRecord,
                AVAudioSessionCategoryOptionDefaultToSpeaker | AVAudioSessionCategoryOptionAllowBluetoothA2DP);
}

void ReleaseVoice() {
  if (!g_voice_active.exchange(false)) {
    return;
  }
  // Same busy retry as a call's Deactivate; it does nothing when a newer activation (a call) has the session.
  DeactivateAttempt(g_session_gen.load(), kDeactivateRetries);
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
  AVAudioSessionCategoryOptions options =
      AVAudioSessionCategoryOptionAllowBluetoothHFP | AVAudioSessionCategoryOptionAllowBluetoothA2DP;
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

} // namespace AudioSession
} // namespace pbr

#endif
