#pragma once

#include "domain/media/IVideoCodec.h"
#include "common/Error.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/** Device kinds a media pipeline can hold (media-client-layers L010 / L011). */
enum class MediaDeviceKind : uint8_t { Mic, Speaker, Camera };

const char* MediaDeviceKindName(MediaDeviceKind kind);

/** Signed 16-bit PCM at `freq` Hz, `channels` interleaved. */
struct AudioDeviceFormat {
  int freq = 48000;
  int channels = 1;
};

/**
 * OS voice processing on a duplex mic + speaker pair (Apple VPIO: echo cancellation, noise
 * suppression, AGC). Diagnostics and route changes for the pipeline holding the pair.
 */
class IVoiceProcessing {
public:
  virtual ~IVoiceProcessing() = default;
  /** Largest render request from the device, in bytes; playout keeps more than this queued. */
  virtual size_t RenderChunkBytes() const = 0;
  /** Playout underruns since the pair opened. */
  virtual uint64_t PlayoutUnderruns() const = 0;
  /** Diagnostic counters since the last call (callback sizes, ring water marks); resets them. */
  virtual std::string TakeDiag() = 0;
  /** The default device pair changed since the last call: re-acquire the pair. */
  virtual bool TakeDeviceChanged() = 0;
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
  /** Set when this endpoint is one half of a voice-processing duplex (OpenVoiceDuplex). */
  virtual IVoiceProcessing* VoiceProcessing() { return nullptr; }
};

/** Both halves of one voice-processing unit; the unit closes when both are destroyed. */
struct VoiceDuplexEndpoints {
  std::unique_ptr<IAudioEndpoint> mic;
  std::unique_ptr<IAudioEndpoint> speaker;
};

/** What a camera holder asks for. */
struct CameraRequestFormat {
  /** Display rotation read on the requester's UI thread (`CameraDisplayRotationDegrees`). */
  int display_rotation_deg = 0;
  int fps = 20;
};

/** How to turn the opened camera's frames upright and what size to encode. */
struct CameraGeometry {
  int rotate_cw = 0;
  int encode_width = 640;
  int encode_height = 360;
  /** Lets the holder re-derive rotate_cw per frame as the display turns (CameraFrameRotateCw). */
  bool front_facing = false;
};

/** One opened camera. NextFrame may run on any thread (one reader at a time). */
class ICameraEndpoint {
public:
  virtual ~ICameraEndpoint() = default;
  virtual CameraGeometry Geometry() const = 0;
  /** Latest sensor frame as RGBA (not yet rotated / cropped); nullopt when none is ready. */
  virtual std::optional<VideoFrameRgba> NextFrame() = 0;
};

/**
 * Opens OS endpoints. Called on the arbiter's device thread only; opens may block (OS mic prompt).
 * Null with `error` set when no device could be opened (headless, permission denied).
 */
class IMediaDeviceBackend {
public:
  virtual ~IMediaDeviceBackend() = default;
  /** An audio lease is still granted without a device. `still_wanted` aborts platform open retries. */
  virtual std::unique_ptr<IAudioEndpoint> OpenAudio(MediaDeviceKind kind, const AudioDeviceFormat& format,
                                                    const std::function<bool()>& still_wanted,
                                                    std::string* error) = 0;
  /** A camera lease is refused without a device. */
  virtual std::unique_ptr<ICameraEndpoint> OpenCamera(const CameraRequestFormat& format, std::string* error) {
    (void)format;
    if (error) {
      *error = "no camera backend";
    }
    return nullptr;
  }
  /**
   * Mic + speaker through one OS voice-processing unit (echo cancellation needs the unit's own
   * playback as its reference). Both null with `error` set when unsupported or the open failed —
   * the caller then takes separate leases.
   */
  virtual VoiceDuplexEndpoints OpenVoiceDuplex(const AudioDeviceFormat& /*format*/, std::string* error) {
    if (error) {
      *error = "unsupported";
    }
    return {};
  }
  /** Pause between closing and reopening `kind` (Android OEM route settle). */
  virtual std::chrono::milliseconds ReopenSettle(MediaDeviceKind /*kind*/) const { return {}; }
};

/** Backend with no devices: audio leases without endpoints, cameras refused (tests, headless tools). */
std::unique_ptr<IMediaDeviceBackend> CreateNullMediaDeviceBackend();
/** SDL3 audio (default recording / playback device) + SDL3 camera (front-facing preferred). */
std::unique_ptr<IMediaDeviceBackend> CreateSdlMediaDeviceBackend();

/** Which kinds admit one holder at a time. Shared kinds mix (the OS mixes playback streams). */
struct MediaDeviceSharePolicy {
  bool exclusive_mic = true;
  bool exclusive_speaker = false;
  bool exclusive_camera = true;

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
   * thread. The lease is kept when the reopen finds no device (HasDevice() false). A voice-duplex
   * half refuses (its unit serves both halves): release the pair and acquire it again.
   */
  Roe<void> Reopen();
  /** Half of a voice-processing duplex (AcquireVoiceDuplex). */
  bool IsVoiceDuplex() const;
  /** Run `fn` on the endpoint's voice processing, if any, under the I/O lock; false when none. */
  bool WithVoiceProcessing(const std::function<void(IVoiceProcessing&)>& fn);

  struct State;

private:
  friend class MediaDeviceArbiter;
  explicit AudioDeviceLease(std::shared_ptr<State> state);
  std::shared_ptr<State> state_;
};

/** A granted camera. Release (destruction) closes it on the device thread without blocking. */
class CameraDeviceLease {
public:
  ~CameraDeviceLease();
  CameraDeviceLease(const CameraDeviceLease&) = delete;
  CameraDeviceLease& operator=(const CameraDeviceLease&) = delete;

  const std::string& Holder() const;
  CameraGeometry Geometry() const;
  std::optional<VideoFrameRgba> NextFrame();

  struct State;

private:
  friend class MediaDeviceArbiter;
  explicit CameraDeviceLease(std::shared_ptr<State> state);
  std::shared_ptr<State> state_;
};

struct CameraLeaseRequest {
  std::string holder;
  CameraRequestFormat format;
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
  explicit MediaDeviceArbiter(std::unique_ptr<IMediaDeviceBackend> backend, MediaDeviceSharePolicy policy = {});
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

  struct VoiceDuplexLeases {
    std::unique_ptr<AudioDeviceLease> mic;
    std::unique_ptr<AudioDeviceLease> speaker;
  };
  /**
   * Blocking like AcquireAudio: mic + speaker from one voice-processing unit (`request.kind` is
   * ignored). Refused by policy (mic held), or when the backend has no such unit / it failed to
   * open (the error says why) — then take separate AcquireAudio leases.
   */
  Roe<VoiceDuplexLeases> AcquireVoiceDuplex(const AudioLeaseRequest& request);
  /** Blocking like AcquireAudio. Refused by policy ("camera held by …") or when no camera opens. */
  Roe<std::unique_ptr<CameraDeviceLease>> AcquireCamera(const CameraLeaseRequest& request);

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
