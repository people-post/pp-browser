#pragma once

#include "lib/media/AudioSession.h"
#include "lib/media/MediaDeviceArbiter.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace pbr {

/**
 * Loops a tone on a speaker lease while Start()'d. The speaker is shared (media-client-layers
 * L011): ringing over an active call (second invite) mixes with call audio; the arbiter's device
 * thread keeps this open / close from racing call-media's.
 *
 * Tone::IncomingRing loops assets/sounds/call_ring.wav. Tone::OutgoingRingback synthesizes the
 * caller ringback (450 Hz 1.0 s on / 4.0 s off) and, on phones, routes it to the earpiece via
 * AudioSession::ActivateForVoip() like an in-call session. For this tone the worker thread
 * itself owns activate/release of the audio session (see RunLoop and SetReleaseSessionOnStop) so
 * a caller-side Stop() can never race a late activate that lands after the UI already decided to
 * release the session. Each Start() hands its worker a fresh, per-generation stop flag, release
 * flag and a "previous worker done" signal, so a fast cancel-then-redial (Start() launching a new
 * worker before the old one has finished tearing down) can never cross-wire generations — see
 * RunLoop for the ordering rules.
 *
 * Tone::IncomingRing follows the phone's ringer (DEV/PLAN-android-incoming-call-screen.md step 4): the app passes in
 * how it wants a ring now (SetOutputPorts; this capability asks no platform), at Start() and at each Refresh(), and the
 * ring sounds, vibrates (through the app's port), both or neither; a change while ringing switches at the next
 * Refresh(). The outgoing ringback is never affected.
 */
class CallRingtone {
public:
  static constexpr std::chrono::milliseconds kDefaultShutdownJoinBudget{500};

  enum class Tone { IncomingRing, OutgoingRingback };

  /** How the phone wants an incoming ring now (the app reads its ringer mode). */
  struct Output {
    bool sound = true;
    bool vibrate = false;
  };
  struct OutputPorts {
    /** Asked at Start() and each Refresh() (UI thread). Unset: sound, no vibration. */
    std::function<Output()> read;
    /** The phone's ring vibration on / off (UI thread). */
    std::function<void(bool on)> vibrate;
  };

  /** The incoming ring's WAV file (the app resolves its asset); none: the incoming ring is silent. */
  static void SetRingSoundPath(std::string path);

  /** Speaker from `devices` (must outlive the ringtone). */
  explicit CallRingtone(Tone tone = Tone::IncomingRing, MediaDeviceArbiter& devices = MediaDeviceArbiter::Default());
  ~CallRingtone();

  CallRingtone(const CallRingtone&) = delete;
  CallRingtone& operator=(const CallRingtone&) = delete;

  /** Tone::IncomingRing only: how the ring follows the phone. Before the first Start(). */
  void SetOutputPorts(OutputPorts ports);

  /** Tone::IncomingRing: rings as OutputPorts::read says now (perhaps silently: the ring page alone). */
  void Start();
  /** Tone::IncomingRing, while ringing: asks OutputPorts::read again and switches sound / vibration to it. */
  void Refresh();
  /** Async (safe from UI/Accept). Does not join — use StopAndJoin before SDL_Quit. Ends the vibration too. */
  void Stop();
  /**
   * Signal stop and join playback + any async joiner (unlimited).
   * Prefer the budgeted overload on product quit.
   */
  void StopAndJoin();
  /**
   * After RequestStop(wait=false), wait up to `budget` for playback+joiner.
   * On timeout detaches with a loud log. Returns false if detach occurred.
   */
  bool StopAndJoin(std::chrono::milliseconds budget);
  /** Tone::IncomingRing: ringing (Start()ed, not stopped), even when the phone wants it silent. Ringback: playing. */
  bool IsPlaying() const { return tone_ == Tone::IncomingRing ? ringing_.load() : playing_.load(); }
  /** The tone itself is playing (Tone::IncomingRing: false while the phone wants it silent). */
  bool IsSounding() const { return playing_.load(); }
  /** Tone::IncomingRing: the phone's ring vibration is on. */
  bool IsVibrating() const { return vibrating_; }
  /**
   * Tone::OutgoingRingback only: whether the CURRENT (most recently Start()'d) worker should
   * release the phone audio session (AudioSession::Deactivate()) when it stops, having
   * activated it. Call before Stop() for that same worker — false when media is taking over the
   * session (the engine owns it from here), true (the default) to release it. Writes to a flag
   * private to that worker's generation, so a still-tearing-down older worker can never pick up a
   * decision meant for the current one.
   */
  void SetReleaseSessionOnStop(bool release);
  /**
   * True while a ringback worker holds the phone audio session it activated and may still
   * Deactivate() it (a tone hold, AudioSession::BeginToneHold). Call media waits for the holds to
   * end before activating its own session, so a late ringback release can never kill a call that just
   * started.
   */
  static bool RingbackSessionHeld() { return AudioSession::ToneHoldsSession(); }
  /**
   * Tone::IncomingRing: a stop takes a tone hold (AudioSession::BeginToneHold) that the worker ends only once its
   * speaker stream is closed on the device thread (bounded by kRingCloseWait), so call media started by an answer waits
   * for that close (B230: on iOS the close rewrites the audio session). Default: AudioSession::ToneCloseRewritesSession().
   * Before the first Start().
   */
  void SetHoldSessionUntilClosed(bool hold) { hold_until_closed_ = hold; }
  /** How long a stopped incoming ring waits for its stream's close before it ends its hold anyway. */
  static constexpr std::chrono::milliseconds kRingCloseWait{2000};

private:
  /** One incoming-ring generation's stop-time hold (SetHoldSessionUntilClosed): who ends it, and when the stop came. */
  struct RingHold {
    static constexpr int kRunning = 0; // no hold
    static constexpr int kHeld = 1;    // a stop took the hold; the worker ends it
    static constexpr int kDone = 2;    // the worker finished: a later stop takes no hold
    std::atomic<int> state{kRunning};
    /** steady_clock nanoseconds of the first stop (0: none yet). */
    std::atomic<int64_t> stop_at_ns{0};
  };
  /** Under mutex_, before the stop flag is set: take the current generation's hold (unless its worker finished). */
  void TakeRingHold();
  /** Worker, after its lease is released: wait for the close, then end the hold a stop took (if any). */
  void FinishRingHold(const std::shared_ptr<RingHold>& hold);
  /** The tone's worker (the old Start()). */
  void StartTone();
  /** Tone::IncomingRing: sound and vibration as `out` says. */
  void Apply(Output out);
  /** Tone::IncomingRing: not ringing any more, and the vibration off. */
  void EndRing();
  void RequestStop(bool wait);
  /**
   * `previous_done`: the prior generation's completion signal (null on the first Start()).
   * `stop`: this generation's own stop signal (see RequestStop). `release_on_stop`: this
   * generation's own release flag (see SetReleaseSessionOnStop). `done`: this generation's own
   * completion signal, set as the last step before returning, for whichever later generation
   * waits on it next.
   */
  void RunLoop(std::shared_ptr<std::atomic<bool>> previous_done, std::shared_ptr<std::atomic<bool>> stop,
               std::shared_ptr<std::atomic<bool>> release_on_stop, std::shared_ptr<std::atomic<bool>> done,
               std::shared_ptr<RingHold> hold);

  MediaDeviceArbiter& devices_;
  std::mutex mutex_;
  std::atomic<bool> playing_{false};
  std::thread thread_;
  /** Joins prior playback workers after async Stop/Start so Accept never blocks on a device close. */
  std::thread joiner_;
  Tone tone_ = Tone::IncomingRing;
  /** Tone::IncomingRing (UI thread): Start()ed and not stopped; whether the vibration is on; the app's ports. */
  std::atomic<bool> ringing_{false};
  bool vibrating_ = false;
  OutputPorts output_;
  /** The last output applied, for one log line per change. */
  bool last_sound_ = false;
  /**
   * Guarded by mutex_: the stop signal, release flag and completion signal for the CURRENT (most
   * recently Start()'d) generation. RequestStop sets current_stop_ (a worker only ever reads its
   * own); SetReleaseSessionOnStop writes through current_release_ (OutgoingRingback only); the
   * next Start() reads current_done_ as that generation's `previous_done` before replacing all
   * three with fresh ones for the new generation.
   */
  std::shared_ptr<std::atomic<bool>> current_stop_;
  std::shared_ptr<std::atomic<bool>> current_release_;
  std::shared_ptr<std::atomic<bool>> current_done_;
  /** Guarded by mutex_: the current generation's stop-time hold (Tone::IncomingRing with hold_until_closed_; else null). */
  std::shared_ptr<RingHold> current_hold_;
  bool hold_until_closed_ = AudioSession::ToneCloseRewritesSession();
  std::vector<unsigned char> wav_pcm_;
  int wav_freq_ = 24000;
  int wav_channels_ = 1;
};

} // namespace pbr
