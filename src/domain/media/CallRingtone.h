#pragma once

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

namespace pbr {

/**
 * Loops a tone on the default playback device while Start()'d.
 * Tone::IncomingRing loops assets/sounds/call_ring.wav (unchanged behavior).
 * Tone::OutgoingRingback synthesizes the caller ringback (450 Hz 1.0 s on /
 * 4.0 s off) and, on phones, routes it to the earpiece via
 * CallAudioSession::ActivateForVoipCall() like an in-call session. For this tone the
 * worker thread itself owns activate/release of the audio session (see RunLoop and
 * SetReleaseSessionOnStop) so a caller-side Stop() can never race a late activate that
 * lands after the UI already decided to release the session.
 */
class CallRingtone {
public:
  static constexpr std::chrono::milliseconds kDefaultShutdownJoinBudget{500};

  enum class Tone { IncomingRing, OutgoingRingback };

  explicit CallRingtone(Tone tone = Tone::IncomingRing);
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
   * Tone::OutgoingRingback only: whether the worker should release the phone audio
   * session (CallAudioSession::Deactivate()) when it stops, having activated it. Call
   * before Stop() — false when media is taking over the session (engine owns it from
   * here), true (the default) to release it. The worker reads this once, at its own
   * exit, so the decision made here can never race the worker's own activate.
   */
  void SetReleaseSessionOnStop(bool release);
  /**
   * True while an SDL playback stream from Start() may still be open.
   * IsPlaying() clears on Stop() before DestroyAudioStream — call-media must wait on this.
   */
  static bool PlaybackDeviceHeld();
  /** Spin-wait (non-UI) until PlaybackDeviceHeld() is false or timeout. */
  static void WaitUntilPlaybackDeviceReleased(int timeout_ms = 2000);

private:
  void RequestStop(bool wait);
  void RunLoop();

  std::mutex mutex_;
  std::atomic<bool> playing_{false};
  std::atomic<bool> stop_{false};
  std::thread thread_;
  /** Joins prior playback workers after async Stop/Start so Accept never blocks on SDL close. */
  std::thread joiner_;
  Tone tone_ = Tone::IncomingRing;
  /** OutgoingRingback only; read by RunLoop at its own exit. See SetReleaseSessionOnStop. */
  std::atomic<bool> release_session_on_stop_{true};
  std::vector<unsigned char> wav_pcm_;
  int wav_freq_ = 24000;
  int wav_channels_ = 1;
};

} // namespace pbr
