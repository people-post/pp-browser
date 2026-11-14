#include "lib/media/AudioSession.h"

#include <TargetConditionals.h>

#if defined(__APPLE__) && TARGET_OS_IPHONE

#import <AVFoundation/AVFoundation.h>
#import <UIKit/UIKit.h>

#include "common/Logger.h"
#include "common/Metrics.h"
#include "lib/media/ResumeRetryChain.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>

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
/** Voice messages hold the session (recording or playback); cleared by ReleaseVoice. */
std::atomic<bool> g_voice_active{false};
/** The voice session is for playing a voice message (set by ActivateForVoicePlayback, cleared by ReleaseVoice). */
std::atomic<bool> g_voice_playback{false};
/** Bumped by every ActivateForVoip; a delayed Deactivate retry gives up once it changed. */
std::atomic<uint64_t> g_session_gen{0};
/**
 * An interruption began while the session was not (yet) marked in use: set by the observer, cleared by ActivateForVoip
 * before its setActive:YES and read right after it marks the session in use (an interruption in that window is real).
 */
std::atomic<bool> g_began_before_marked{false};
/** Makes "still ours?" + setActive:NO atomic against a new activation / CancelPendingDeactivate. */
std::mutex g_session_mu;
constexpr int kDeactivateRetries = 5;
constexpr int64_t kDeactivateRetryNs = 150 * NSEC_PER_MSEC;

#if defined(PP_BROWSER_DEV_RESUME_RETRY_ONLY) && PP_BROWSER_DEV_RESUME_RETRY_ONLY
/**
 * Local dev builds only (DEV/tools/install-iphone.sh RETRY_ONLY=1; CI and TestFlight never define it): the
 * "interruption ended" and app-became-active resumes only log, so the retry chain is tested alone (B217).
 */
constexpr bool kRetryOnly = true;
#else
constexpr bool kRetryOnly = false;
#endif

using AppState = ResumeRetryChain::AppState;
/** UIApplication's state, from its notifications (main thread); read on the resume queue. Unknown until seen. */
std::atomic<AppState> g_app_state{AppState::Unknown};

/** What resumed the session (call.audio_resume by=…). */
enum class ResumeBy { Retry, Ended, Foreground, Route, Reset };

/**
 * Every resume attempt runs on this one serial queue (TryResume), so attempts never overlap and the retry chain needs
 * no lock. Nothing ever waits on it (dispatch_async / dispatch_after only).
 */
dispatch_queue_t ResumeQueue() {
  static dispatch_queue_t queue = dispatch_queue_create("pp.audio.resume", DISPATCH_QUEUE_SERIAL);
  return queue;
}

/** On ResumeQueue: one resume attempt by something other than the retry chain. */
void RunResume(ResumeBy by, bool reapply);
/** On ResumeQueue: an interruption began at `began_at`; starts the retry chain unless one runs. */
void BeginRetryChain(std::chrono::steady_clock::time_point began_at);
/** On ResumeQueue: forget when the last interruption began (the next resume's waited_ms is then 0). */
void ForgetBegan();

/**
 * An interruption of the session activated as `gen` began at `began_at`: on the resume queue, so it is ordered with
 * the resume attempts (an attempt that is between its setActive:YES and its NotifyInterruptionEnded finishes first,
 * then this marks the session interrupted again — never the other way round, which would lose it), mark it
 * interrupted and start the retry chain. Dropped when the call ended or a newer activation took the session by then.
 */
void PostInterruptionBegan(const std::chrono::steady_clock::time_point began_at, const uint64_t gen) {
  dispatch_async(ResumeQueue(), ^{
    if (!g_session_active.load() || g_session_gen.load() != gen) {
      return;
    }
    NotifyInterruptionBegan();
    BeginRetryChain(began_at);
  });
}

void PostResume(const ResumeBy by, const bool reapply) {
  dispatch_async(ResumeQueue(), ^{
    RunResume(by, reapply);
  });
}

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
    case 0:
      return "default";
    case 1:
      return "app_was_suspended";
    case 2:
      return "built_in_mic_muted";
    case 4:
      return "route_disconnected"; // iOS 17+: no "ended" follows
    default:
      return "other";
  }
}

ShareWord RouteChangeReasonWord(AVAudioSessionRouteChangeReason reason) {
  switch (reason) {
    case AVAudioSessionRouteChangeReasonNewDeviceAvailable:
      return "NewDeviceAvailable";
    case AVAudioSessionRouteChangeReasonOldDeviceUnavailable:
      return "OldDeviceUnavailable";
    case AVAudioSessionRouteChangeReasonCategoryChange:
      return "CategoryChange";
    case AVAudioSessionRouteChangeReasonOverride:
      return "Override";
    case AVAudioSessionRouteChangeReasonWakeFromSleep:
      return "WakeFromSleep";
    case AVAudioSessionRouteChangeReasonNoSuitableRouteForCategory:
      return "NoSuitableRouteForCategory";
    case AVAudioSessionRouteChangeReasonRouteConfigurationChange:
      return "RouteConfigurationChange";
    default:
      return "Unknown";
  }
}

/** A port type as a word (never a port name: that can be a person's "AirPods of …"). */
ShareWord PortWord(NSString* port) {
  if (port == nil)
    return "none";
  if ([port isEqualToString:AVAudioSessionPortBuiltInReceiver])
    return "Receiver";
  if ([port isEqualToString:AVAudioSessionPortBuiltInSpeaker])
    return "Speaker";
  if ([port isEqualToString:AVAudioSessionPortHeadphones])
    return "Headphones";
  if ([port isEqualToString:AVAudioSessionPortBluetoothHFP])
    return "BluetoothHFP";
  if ([port isEqualToString:AVAudioSessionPortBluetoothA2DP])
    return "BluetoothA2DPOutput";
  if ([port isEqualToString:AVAudioSessionPortBluetoothLE])
    return "BluetoothLE";
  if ([port isEqualToString:AVAudioSessionPortUSBAudio])
    return "USBAudio";
  if ([port isEqualToString:AVAudioSessionPortLineOut])
    return "LineOut";
  if ([port isEqualToString:AVAudioSessionPortHDMI])
    return "HDMI";
  if ([port isEqualToString:AVAudioSessionPortAirPlay])
    return "AirPlay";
  if ([port isEqualToString:AVAudioSessionPortCarAudio])
    return "CarAudio";
  return "other";
}

/** The current output's port type. */
ShareWord OutputPortWord() {
  AVAudioSessionRouteDescription* route = [AVAudioSession sharedInstance].currentRoute;
  return PortWord(route.outputs.count > 0 ? route.outputs.firstObject.portType : nil);
}

/** Every current output as `Type:Name,…` for the developer log only (the name stays out of metrics). */
std::string OutputsForLog() {
  AVAudioSessionRouteDescription* route = [AVAudioSession sharedInstance].currentRoute;
  if (route.outputs.count == 0) {
    return "none";
  }
  std::string text;
  for (AVAudioSessionPortDescription* output in route.outputs) {
    if (!text.empty()) {
      text += ",";
    }
    text += std::string(PortWord(output.portType).view()) + ":" + Utf8(output.portName);
  }
  return text;
}

/**
 * The device the session was using went away while the OS had the session: iOS sends no "ended" for that
 * (route_disconnected), and the app stays in the foreground, so nothing else resumes — retry shortly, and once more.
 * A resume only happens when the session can be had (a phone call keeps it), tried with setActive. On the resume
 * queue: the second try sees the first's resume.
 */
void ResumeSoonIfSessionFree() {
  for (const int64_t ms : {300, 1000}) {
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, ms * NSEC_PER_MSEC), ResumeQueue(), ^{
      RunResume(ResumeBy::Route, /*reapply=*/false);
    });
  }
}

AppState AppStateFromUIKit(const UIApplicationState state) {
  switch (state) {
    case UIApplicationStateActive:
      return AppState::Active;
    case UIApplicationStateInactive:
      return AppState::Inactive;
    case UIApplicationStateBackground:
      return AppState::Background;
  }
  return AppState::Unknown;
}

/**
 * Once, at the first live session: an interruption that begins marks the session interrupted (NotifyInterruptionBegan)
 * and starts the retry chain (iOS does not always send an end: Siri sends none); its end, a media-services reset, the
 * app becoming active again, or the chain's next try resume the session (TryResume, on the resume queue) and run every
 * AudioSession interruption handler (AudioSessionInterruptions.cpp). The app's state is kept for the chain.
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
                        if (kRetryOnly) {
                          SessionLog().info << "AudioSession interruption ended: ignored (retry only)";
                        } else {
                          PostResume(ResumeBy::Ended, /*reapply=*/false);
                        }
                      } else if (type == AVAudioSessionInterruptionTypeBegan) {
                        const auto began_at = std::chrono::steady_clock::now();
                        if (!g_session_active.load()) {
                          // Maybe ActivateForVoip is between its setActive:YES and marking the session in use: leave a
                          // note for it. Look again afterwards, in case it marked the session in use meanwhile.
                          g_began_before_marked.store(true);
                          if (!g_session_active.load()) {
                            return;
                          }
                        }
                        NSNumber* reason_value = note.userInfo[AVAudioSessionInterruptionReasonKey];
                        const ShareWord reason = reason_value
                                                     ? InterruptionReasonWord(reason_value.unsignedIntegerValue)
                                                     : ShareWord("none");
                        SessionLog().info << "AudioSession interruption began reason=" << reason.view();
                        PostInterruptionBegan(began_at, g_session_gen.load());
                        if (reason_value && reason_value.unsignedIntegerValue == 4) { // route_disconnected
                          ResumeSoonIfSessionFree();
                        }
                      }
                    }];
    [center addObserverForName:AVAudioSessionRouteChangeNotification
                        object:nil
                         queue:nil
                    usingBlock:^(NSNotification* note) {
                      const bool call = g_session_active.load();
                      if (!call && !g_voice_playback.load()) {
                        return; // a call's route (B178), or a voice message being played
                      }
                      const auto reason = static_cast<AVAudioSessionRouteChangeReason>(
                          [note.userInfo[AVAudioSessionRouteChangeReasonKey] unsignedIntegerValue]);
                      const ShareWord reason_word = RouteChangeReasonWord(reason);
                      const ShareWord output = OutputPortWord();
                      SessionLog().info << "AudioSession route change reason=" << reason_word.view()
                                        << " output=" << output.view() << " interrupted=" << (IsInterrupted() ? 1 : 0)
                                        << " use=" << (call ? "call" : "voice_playback")
                                        << " outputs=" << OutputsForLog();
                      MetricsLine("audio.route")
                          .Add("reason", reason_word)
                          .Add("output", output)
                          .Add("use", call ? ShareWord("call") : ShareWord("voice_playback"))
                          .Emit();
                      if (call && reason == AVAudioSessionRouteChangeReasonOldDeviceUnavailable && IsInterrupted()) {
                        ResumeSoonIfSessionFree();
                      }
                    }];
    [center addObserverForName:UIApplicationDidBecomeActiveNotification
                        object:nil
                         queue:nil
                    usingBlock:^(NSNotification*) {
                      g_app_state.store(AppState::Active);
                      if (!IsInterrupted()) {
                        return;
                      }
                      if (kRetryOnly) {
                        SessionLog().info << "AudioSession app active: ignored (retry only)";
                      } else {
                        PostResume(ResumeBy::Foreground, /*reapply=*/false);
                      }
                    }];
    [center addObserverForName:UIApplicationWillResignActiveNotification
                        object:nil
                         queue:nil
                    usingBlock:^(NSNotification*) {
                      g_app_state.store(AppState::Inactive);
                    }];
    [center addObserverForName:UIApplicationDidEnterBackgroundNotification
                        object:nil
                         queue:nil
                    usingBlock:^(NSNotification*) {
                      g_app_state.store(AppState::Background);
                    }];
    [center addObserverForName:UIApplicationWillEnterForegroundNotification
                        object:nil
                         queue:nil
                    usingBlock:^(NSNotification*) {
                      g_app_state.store(AppState::Inactive); // DidBecomeActive follows
                    }];
    [center addObserverForName:AVAudioSessionMediaServicesWereResetNotification
                        object:nil
                         queue:nil
                    usingBlock:^(NSNotification*) {
                      PostResume(ResumeBy::Reset, /*reapply=*/true);
                    }];
    // The first live session starts long after launch (and off the main thread): no DidBecomeActive is coming, so read
    // the state once. A notification seen first wins: this only replaces Unknown.
    dispatch_async(dispatch_get_main_queue(), ^{
      AppState expected = AppState::Unknown;
      g_app_state.compare_exchange_strong(expected,
                                          AppStateFromUIKit([UIApplication sharedApplication].applicationState));
    });
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

/** The live session's category, mode and preferred format (errors ignored, as before: setActive is what counts). */
void ConfigureVoipSession(AVAudioSession* session) {
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
  (void)error;
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
  ConfigureVoipSession(session);
  NSError* error = nil;
  BOOL active_ok = NO;
  uint64_t gen = 0;
  {
    std::lock_guard<std::mutex> lock(g_session_mu);
    gen = g_session_gen.fetch_add(1) + 1;
    g_began_before_marked.store(false);
    active_ok = [session setActive:YES error:&error];
    g_session_active.store(true);
    // The call has the session: a voice message that was playing was cut off without ReleaseVoice (a call holds the
    // devices then), and its flag would label every later route change as voice playback.
    g_voice_playback.store(false);
  }
  if (!active_ok) {
    SessionLog().warning << "AudioSession ActivateForVoip: setActive failed code="
                         << (error != nil ? static_cast<long>(error.code) : 0L)
                         << " interrupted=" << (IsInterrupted() ? 1 : 0);
  }
  // The OS holds the session already (Siri, a phone call, another app: no "began" will come, it happened before the
  // session was marked in use), or took it between setActive:YES and the mark: either way the call starts interrupted.
  if (!active_ok || g_began_before_marked.exchange(false)) {
    PostInterruptionBegan(std::chrono::steady_clock::now(), gen);
  }
  ApplyRoute(g_speakerphone.load());
  LogRoute("ActivateForVoip");
}

void Deactivate() {
  g_session_active.store(false);
  if (IsInterrupted()) {
    // The call ended while the OS still had its audio: nothing gave it back (B217).
    SessionLog().info << "AudioSession Deactivate: still interrupted";
    MetricsLine("call.audio_resume").Add("result", "ended_interrupted").Emit();
  }
  ClearInterruption();
  dispatch_async(ResumeQueue(), ^{
    ForgetBegan();
  });
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

using Clock = std::chrono::steady_clock;
using TryResult = ResumeRetryChain::TryResult;

ShareWord ByWord(const ResumeBy by) {
  switch (by) {
    case ResumeBy::Retry:
      return "retry";
    case ResumeBy::Ended:
      return "ended";
    case ResumeBy::Foreground:
      return "foreground";
    case ResumeBy::Route:
      return "route";
    case ResumeBy::Reset:
      return "reset";
  }
  return "other";
}

const char* ResultWord(const TryResult result) {
  switch (result) {
    case TryResult::Ok:
      return "ok";
    case TryResult::Failed:
      return "failed";
    case TryResult::Stale:
      return "stale";
    case TryResult::Gone:
      return "gone";
    case TryResult::Already:
      return "already";
  }
  return "other";
}

/** The retry chain. Only ever touched on ResumeQueue (no lock). */
ResumeRetryChain& RetryChain() {
  static ResumeRetryChain chain;
  return chain;
}

/** One TryResume, for its log line. */
struct ResumeAttempt {
  TryResult result = TryResult::Gone;
  /** setActive:YES ran (else nothing was there to resume). */
  bool tried = false;
  bool active_ok = false;
  long error_code = 0;
  bool other_audio = false;
  bool silence_hint = false;
  uint64_t gen = 0;
  AppState app = AppState::Unknown;
};

void LogAttempt(const ResumeBy by, const ResumeAttempt& attempt, const int failures = 0) {
  const std::string failures_text = failures > 0 ? " failures=" + std::to_string(failures) : std::string();
  SessionLog().info << "AudioSession resume by=" << ByWord(by).view() << " gen=" << attempt.gen
                    << " app=" << ResumeRetryChain::Word(attempt.app)
                    << " set_active=" << (attempt.active_ok ? "ok" : "failed") << " code=" << attempt.error_code
                    << " other_audio=" << (attempt.other_audio ? 1 : 0)
                    << " silence_hint=" << (attempt.silence_hint ? 1 : 0) << " result=" << ResultWord(attempt.result)
                    << failures_text;
}

/**
 * One try to take the live session back after the OS interrupted it (the OS also stopped the audio units), then let
 * media reopen (NotifyInterruptionEnded). On ResumeQueue only. Takes no lock and does not call ActivateForVoip (its
 * flag write and gen bump could re-mark an ended call's session as live). `reapply` (a media-services reset: the
 * session's settings are gone, and it needs the reopen whether or not an interruption was seen) sets the category and
 * mode again and skips the "still interrupted?" checks. Every attempt sets the category, mode, preferred sample rate and
 * IO buffer again before setActive:YES (as ActivateForVoip did before the queue): the OS may have reset them. Not under
 * g_session_mu (SetSpeakerphoneOn does the same without it).
 *
 * Invariant: once Deactivate has returned, no path leaves the session active. Deactivate clears g_session_active
 * before its setActive:NO (DeactivateAttempt), and this checks the flag after its own setActive:YES — before the gen,
 * because a newer activation followed by a hang-up changes the gen too. Still set: Deactivate's setActive:NO comes
 * after this setActive:YES (it cannot have started yet), so it wins. Already clear: this undoes its own activation
 * (DeactivateAttempt, which stands back if anything activated since — that one owns the session now). A voice clip
 * that took the session after the hang-up is left alone: its ReleaseVoice ends it. The undo uses the gen read at the
 * START of the attempt (Deactivate does not bump it; every newer activation does, under the lock), so an activation
 * between our setActive:YES and the undo makes DeactivateAttempt stand down. Nothing waits on this queue, and
 * no path takes g_session_mu twice, so none of this can deadlock.
 *
 * Every setActive:YES result is logged here, except a retry's failure: the chain logs those (1st, then every 10th).
 */
ResumeAttempt TryResume(const ResumeBy by, const bool reapply) {
  ResumeAttempt attempt;
  attempt.gen = g_session_gen.load();
  attempt.app = g_app_state.load();
  if (!g_session_active.load() || (!reapply && !IsInterrupted())) {
    attempt.result = TryResult::Gone;
    return attempt;
  }
  AVAudioSession* session = [AVAudioSession sharedInstance];
  ConfigureVoipSession(session);
  NSError* error = nil;
  attempt.tried = true;
  attempt.active_ok = [session setActive:YES error:&error];
  attempt.error_code = error != nil ? static_cast<long>(error.code) : 0L;
  attempt.other_audio = session.otherAudioPlaying;
  attempt.silence_hint = session.secondaryAudioShouldBeSilencedHint;
  if (!attempt.active_ok) {
    attempt.result = TryResult::Failed;
  } else if (!g_session_active.load()) {
    if (!g_voice_active.load()) {
      DeactivateAttempt(attempt.gen, kDeactivateRetries);
    }
    attempt.result = TryResult::Gone;
  } else if (g_session_gen.load() != attempt.gen) {
    attempt.result = TryResult::Stale; // a newer activation took over
  } else if (!reapply && !IsInterrupted()) {
    attempt.result = TryResult::Already;
  } else {
    ApplyRoute(g_speakerphone.load());
    LogRoute("resume");
    NotifyInterruptionEnded(); // no lock held; the handlers only set flags
    attempt.result = TryResult::Ok;
  }
  if (by != ResumeBy::Retry || attempt.result != TryResult::Failed) {
    LogAttempt(by, attempt);
  }
  return attempt;
}

void EmitResumed(const ResumeBy by, const int tries, const std::chrono::milliseconds waited) {
  SessionLog().info << "AudioSession resumed by=" << ByWord(by).view() << " tries=" << tries
                    << " waited_ms=" << waited.count();
  MetricsLine("call.audio_resume")
      .Add("result", "ok")
      .Add("by", ByWord(by))
      .Add("tries", tries)
      .Add("waited_ms", waited)
      .Emit();
}

void RunTick();

void ScheduleTick() {
  dispatch_after(
      dispatch_time(DISPATCH_TIME_NOW, static_cast<int64_t>(ResumeRetryChain::kInterval.count()) * NSEC_PER_MSEC),
      ResumeQueue(), ^{
        RunTick();
      });
}

void LogChainStop(const ResumeRetryChain::StopReason reason) {
  SessionLog().info << "AudioSession retry chain stopped reason=" << ResumeRetryChain::Word(reason);
}

/** On ResumeQueue: the chain's tick. One tick is pending at a time, and only while the chain runs. */
void RunTick() {
  ResumeRetryChain& chain = RetryChain();
  const auto tick = chain.OnTick(g_session_active.load(), IsInterrupted(), g_app_state.load());
  switch (tick.action) {
    case ResumeRetryChain::Action::Stop:
      LogChainStop(tick.stop);
      return;
    case ResumeRetryChain::Action::Skip:
      if (tick.log_skip || kRetryOnly) {
        SessionLog().info << "AudioSession retry skipped: app inactive skips=" << tick.skips;
      }
      ScheduleTick();
      return;
    case ResumeRetryChain::Action::Try:
      break;
  }
  const ResumeAttempt attempt = TryResume(ResumeBy::Retry, /*reapply=*/false);
  const auto outcome = chain.OnTryResult(attempt.result, Clock::now());
  if (attempt.result == TryResult::Failed && (outcome.log_failure || kRetryOnly)) {
    LogAttempt(ResumeBy::Retry, attempt, outcome.failures);
  }
  if (attempt.result == TryResult::Ok) {
    EmitResumed(ResumeBy::Retry, outcome.tries, outcome.waited);
  }
  if (outcome.reschedule) {
    ScheduleTick();
  } else {
    LogChainStop(outcome.stop);
  }
}

void ForgetBegan() {
  RetryChain().ClearBegan();
}

void BeginRetryChain(const Clock::time_point began_at) {
  if (RetryChain().OnBegan(began_at)) {
    SessionLog().info << "AudioSession retry chain started";
    ScheduleTick();
  }
}

void RunResume(const ResumeBy by, const bool reapply) {
  const ResumeAttempt attempt = TryResume(by, reapply);
  if (attempt.result == TryResult::Ok) {
    EmitResumed(by, 0, RetryChain().SinceBegan(Clock::now()));
    ForgetBegan();
  } else if (!attempt.tried && g_session_active.load()) {
    SessionLog().info << "AudioSession resume by=" << ByWord(by).view() << ": not interrupted, nothing to do";
  }
  if (by == ResumeBy::Reset && attempt.result == TryResult::Failed && g_session_active.load()) {
    // The reset took the session and it cannot be had now: on hold until the chain gets it back.
    if (!IsInterrupted()) {
      NotifyInterruptionBegan();
    }
    BeginRetryChain(Clock::now());
  }
}

} // namespace

namespace {

/** "ok", or "failed(<NSError code>)" for the log. */
std::string Outcome(const BOOL ok, NSError* error) {
  return ok ? "ok" : "failed(" + std::to_string(error != nil ? static_cast<long>(error.code) : 0L) + ")";
}

/** Voice message audio. Not while a call (or ringback) holds the session; cancels a call's pending delayed Deactivate. */
void ActivateVoice(AVAudioSessionCategory category, AVAudioSessionCategoryOptions options, const bool playback) {
  AVAudioSession* session = [AVAudioSession sharedInstance];
  NSError* error = nil;
  std::lock_guard<std::mutex> lock(g_session_mu);
  if (g_session_active.load()) {
    return;
  }
  g_session_gen.fetch_add(1);
  const BOOL category_ok = [session setCategory:category mode:AVAudioSessionModeDefault options:options error:&error];
  const std::string category_result = Outcome(category_ok, error);
  if (!category_ok) {
    NSLog(@"AudioSession voice: setCategory failed: %@", error);
    error = nil;
  }
  const BOOL active_ok = [session setActive:YES error:&error];
  if (!active_ok) {
    NSLog(@"AudioSession voice: setActive failed: %@", error);
  }
  g_voice_active.store(true);
  if (playback) {
    g_voice_playback.store(true);
    // What iOS ended up with, and where it sends the sound: silence on a car stereo or headphones is read from this.
    SessionLog().info << "AudioSession voice playback activated: category=" << Utf8(session.category)
                      << " mode=" << Utf8(session.mode)
                      << " options=" << static_cast<unsigned long>(session.categoryOptions)
                      << " set_category=" << category_result << " set_active=" << Outcome(active_ok, error)
                      << " outputs=" << OutputsForLog();
  }
}

} // namespace

// `playback` ignores the ring/silent switch and uses the loudspeaker when nothing else is plugged in.
void ActivateForVoicePlayback() {
  ActivateVoice(AVAudioSessionCategoryPlayback, 0, /*playback=*/true);
}

void ActivateForVoiceRecording() {
  ActivateVoice(AVAudioSessionCategoryPlayAndRecord,
                AVAudioSessionCategoryOptionDefaultToSpeaker | AVAudioSessionCategoryOptionAllowBluetoothA2DP,
                /*playback=*/false);
}

void ReleaseVoice() {
  g_voice_playback.store(false);
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

void ForgetSpeakerChoice() {
  SetSpeakerphoneOn(false);
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
