#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace pbr {

/**
 * Call audio IO through the OS voice-processing unit (Apple kAudioUnitSubType_VoiceProcessingIO:
 * echo cancellation + noise suppression + AGC). Capture and playback both go through it, because
 * the unit uses its own playback signal as the echo reference. 48 kHz mono s16.
 *
 * Threading: Open/Close/ReadCapture on the engine capture thread; WritePlayout/QueuedPlayoutBytes
 * on the playout thread, only while open (the engine guards this under its mutex). Other
 * platforms get a stub whose Open() fails with reason "unsupported".
 */
class VoiceProcessingIo {
public:
  VoiceProcessingIo();
  ~VoiceProcessingIo();
  VoiceProcessingIo(const VoiceProcessingIo&) = delete;
  VoiceProcessingIo& operator=(const VoiceProcessingIo&) = delete;

  bool Open(std::string* reason);
  void Close();
  bool IsOpen() const;
  size_t ReadCapture(int16_t* out, size_t max_samples);
  size_t WritePlayout(const int16_t* pcm, size_t samples);
  size_t QueuedPlayoutBytes() const;
  bool TakeDeviceChanged();
  uint64_t PlayoutUnderruns() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace pbr
