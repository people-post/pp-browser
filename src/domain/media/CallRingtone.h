#pragma once

#include "domain/media/MediaDeviceArbiter.h"

#include <atomic>
#include <chrono>
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
 * CallAudioSession::ActivateForVoipCall() like an in-call session. For this tone the worker thread
 * itself owns activate/release of the audio session (see RunLoop and SetReleaseSessionOnStop) so
 * a caller-side Stop() can never race a late activate that lands after the UI already decided to
 * release the session. Each Start() hands its worker a fresh, per-generation stop flag, release
 * flag and a "previous worker done" signal, so a fast cancel-then-redial (Start() launching a new
 * worker before the old one has finished tearing down) can never cross-wire generations — see
 * RunLoop for the ordering rules.
 */
class CallRingtone {
public:
  static constexpr std::chrono::milliseconds kDefaultShutdownJoinBudget{500};

  enum class Tone { IncomingRing, OutgoingRingback };

  /** Speaker from `devices` (must outlive the ringtone). */
  explicit CallRingtone(Tone tone = Tone::IncomingRing, MediaDeviceArbiter& devices = MediaDeviceArbiter::Default());
  ~CallRingtone();

  CallRingtone(const CallRingtone&) = delete;
  CallRingtone& operator=(const CallRingtone&) = delete;

  void Start();
  /** Async (safe from UI/Accept). Does not join — use StopAndJoin before SDL_Quit. */
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
  bool IsPlaying() const { return playing_.load(); }
  /**
   * Tone::OutgoingRingback only: whether the CURRENT (most recently Start()'d) worker should
   * release the phone audio session (CallAudioSession::Deactivate()) when it stops, having
   * activated it. Call before Stop() for that same worker — false when media is taking over the
   * session (the engine owns it from here), true (the default) to release it. Writes to a flag
   * private to that worker's generation, so a still-tearing-down older worker can never pick up a
   * decision meant for the current one.
   */
  void SetReleaseSessionOnStop(bool release);
  /**
   * True while a ringback worker holds the phone audio session it activated and may still
   * Deactivate() it. Call media waits for this before activating its own session, so a late
   * ringback release can never kill a call that just started.
   */
  static bool RingbackSessionHeld();
  /** Wait (never on UI) until RingbackSessionHeld() is false or `timeout` passes. */
  static void WaitUntilRingbackSessionReleased(std::chrono::milliseconds timeout = std::chrono::milliseconds(2000));

private:
  void RequestStop(bool wait);
  /**
   * `previous_done`: the prior generation's completion signal (null on the first Start()).
   * `stop`: this generation's own stop signal (see RequestStop). `release_on_stop`: this
   * generation's own release flag (see SetReleaseSessionOnStop). `done`: this generation's own
   * completion signal, set as the last step before returning, for whichever later generation
   * waits on it next.
   */
  void RunLoop(std::shared_ptr<std::atomic<bool>> previous_done, std::shared_ptr<std::atomic<bool>> stop,
               std::shared_ptr<std::atomic<bool>> release_on_stop, std::shared_ptr<std::atomic<bool>> done);

  MediaDeviceArbiter& devices_;
  std::mutex mutex_;
  std::atomic<bool> playing_{false};
  std::thread thread_;
  /** Joins prior playback workers after async Stop/Start so Accept never blocks on a device close. */
  std::thread joiner_;
  Tone tone_ = Tone::IncomingRing;
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
  std::vector<unsigned char> wav_pcm_;
  int wav_freq_ = 24000;
  int wav_channels_ = 1;
};

} // namespace pbr
