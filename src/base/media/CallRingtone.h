#pragma once

#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

namespace pbr {

/** Loops assets/sounds/call_ring.wav on the default playback device while Start()'d. */
class CallRingtone {
public:
  CallRingtone();
  ~CallRingtone();

  CallRingtone(const CallRingtone&) = delete;
  CallRingtone& operator=(const CallRingtone&) = delete;

  void Start();
  /** Async (safe from UI/Accept). Does not join — use StopAndJoin before SDL_Quit. */
  void Stop();
  /** Signal stop and join playback + any async joiner. Call on the UI thread before Backend::Shutdown. */
  void StopAndJoin();
  bool IsPlaying() const { return playing_.load(); }

private:
  void RequestStop(bool wait);
  void RunLoop();

  std::mutex mutex_;
  std::atomic<bool> playing_{false};
  std::atomic<bool> stop_{false};
  std::thread thread_;
  /** Joins prior playback workers after async Stop/Start so Accept never blocks on SDL close. */
  std::thread joiner_;
  std::vector<unsigned char> wav_pcm_;
  int wav_freq_ = 24000;
  int wav_channels_ = 1;
};

} // namespace pbr
