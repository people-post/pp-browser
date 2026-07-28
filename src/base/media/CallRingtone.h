#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
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
  void Stop();
  bool IsPlaying() const { return playing_.load(); }

private:
  void RunLoop();

  std::mutex mutex_;
  std::atomic<bool> playing_{false};
  std::atomic<bool> stop_{false};
  std::thread thread_;
  std::vector<unsigned char> wav_pcm_;
  int wav_freq_ = 24000;
  int wav_channels_ = 1;
};

} // namespace pbr
