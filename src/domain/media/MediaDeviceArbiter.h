#pragma once

#include "common/Error.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/** Device kinds a media pipeline can hold (camera joins in l3b-2 — media-client-layers L010). */
enum class MediaDeviceKind : uint8_t { Mic, Speaker };

const char* MediaDeviceKindName(MediaDeviceKind kind);

/** Signed 16-bit PCM at `freq` Hz, `channels` interleaved. */
struct AudioDeviceFormat {
  int freq = 48000;
  int channels = 1;
};

/** One opened OS audio endpoint. Read / Write / Queued / Clear may run on any thread. */
class IAudioEndpoint {
public:
  virtual ~IAudioEndpoint() = default;
  /** Mic: copy up to `bytes` captured PCM; returns bytes copied (0 = nothing yet, <0 = error). */
  virtual int Read(void* dst, int bytes) = 0;
  /** Speaker: queue PCM for playback. */
  virtual bool Write(const void* src, int bytes) = 0;
  virtual int Queued() const = 0;
  virtual void Clear() = 0;
};

/** Opens OS endpoints. Called on the arbiter's device thread only; Open may block (OS mic prompt). */
class IAudioDeviceBackend {
public:
  virtual ~IAudioDeviceBackend() = default;
  /**
   * Null with `error` set when no device could be opened (headless, permission denied) — the lease
   * is still granted without a device. `still_wanted` aborts platform open retries.
   */
  virtual std::unique_ptr<IAudioEndpoint> Open(MediaDeviceKind kind, const AudioDeviceFormat& format,
                                               const std::function<bool()>& still_wanted,
                                               std::string* error) = 0;
  /** Pause between closing and reopening `kind` (Android OEM route settle). */
  virtual std::chrono::milliseconds ReopenSettle(MediaDeviceKind /*kind*/) const { return {}; }
};

/** Backend with no devices: every lease is granted without an endpoint (tests, headless tools). */
std::unique_ptr<IAudioDeviceBackend> CreateNullAudioDeviceBackend();
/** SDL3 audio (default recording / playback device). */
std::unique_ptr<IAudioDeviceBackend> CreateSdlAudioDeviceBackend();

/** Which kinds admit one holder at a time. Shared kinds mix (the OS mixes playback streams). */
struct MediaDeviceSharePolicy {
  bool exclusive_mic = true;
  bool exclusive_speaker = false;

  bool Exclusive(MediaDeviceKind kind) const;
};

class MediaDeviceArbiter;

/**
 * A granted hold on one device kind; releasing it (destruction) closes the endpoint on the device
 * thread without blocking the caller. I/O goes through the lease so the endpoint can never be
 * closed underneath a reader / writer; while a Reopen is in flight I/O sees no device.
 */
class AudioDeviceLease {
public:
  ~AudioDeviceLease();
  AudioDeviceLease(const AudioDeviceLease&) = delete;
  AudioDeviceLease& operator=(const AudioDeviceLease&) = delete;

  MediaDeviceKind Kind() const;
  const std::string& Holder() const;
  /** Endpoint open (false: none present / open failed / reopen in flight). */
  bool HasDevice() const;
  /** Why the last open produced no device (empty when it did). */
  std::string OpenError() const;

  int Read(void* dst, int bytes);
  bool Write(const void* src, int bytes);
  int Queued() const;
  void Clear();

  /**
   * Close, settle, reopen on the device thread; blocks the caller until done. Never call on the UI
   * thread. The lease is kept when the reopen finds no device (HasDevice() false).
   */
  Roe<void> Reopen();

  struct State;

private:
  friend class MediaDeviceArbiter;
  explicit AudioDeviceLease(std::shared_ptr<State> state);
  std::shared_ptr<State> state_;
};

struct AudioLeaseRequest {
  MediaDeviceKind kind = MediaDeviceKind::Speaker;
  /** Free label for logs and refusals (session id, "ringtone"). */
  std::string holder;
  AudioDeviceFormat format;
  /** Polled during platform open retries; false abandons the open (lease granted without device). */
  std::function<bool()> still_wanted;
};

/**
 * Owns OS audio devices for every media pipeline in the process (media-client-layers L010/L011):
 * one device thread performs every open / close / reopen, so an open never races another holder's
 * close (the ringtone ↔ call-media Samsung hang), and exclusive kinds are refused with the holder's
 * name instead of silently doubled.
 *
 * Injectable: pipelines take a `MediaDeviceArbiter&`; `Default()` is the process instance (SDL).
 */
class MediaDeviceArbiter {
public:
  explicit MediaDeviceArbiter(std::unique_ptr<IAudioDeviceBackend> backend, MediaDeviceSharePolicy policy = {});
  ~MediaDeviceArbiter();
  MediaDeviceArbiter(const MediaDeviceArbiter&) = delete;
  MediaDeviceArbiter& operator=(const MediaDeviceArbiter&) = delete;

  /** Process default (SDL backend, default policy). */
  static MediaDeviceArbiter& Default();
  /** `Default().Shutdown(budget)` when the default was ever created (product quit, before SDL_Quit). */
  static bool ShutdownDefault(std::chrono::milliseconds budget);

  /**
   * Blocking: waits for the device thread to open the endpoint. Never call on the UI thread (OS mic
   * permission can block for seconds). Refused only by policy ("mic held by <holder>").
   */
  Roe<std::unique_ptr<AudioDeviceLease>> AcquireAudio(const AudioLeaseRequest& request);

  /** Current holders of `kind` (empty when free). */
  std::vector<std::string> Holders(MediaDeviceKind kind) const;

  /**
   * Run queued closes (released leases) and stop the device thread; wait at most `budget`.
   * Call before SDL_Quit. Leases acquired afterwards open and close inline on the caller.
   * Returns false when the budget ran out (the thread is detached with a loud log).
   */
  bool Shutdown(std::chrono::milliseconds budget);

  struct Core;

private:
  std::shared_ptr<Core> core_;
};

} // namespace pbr
