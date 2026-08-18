#pragma once

#include "common/Error.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace pbr {

/** Planar I420 frame (WebRTC / HW encoder friendly). */
struct VideoFrameI420 {
  int width = 0;
  int height = 0;
  std::vector<uint8_t> y;
  std::vector<uint8_t> u;
  std::vector<uint8_t> v;
};

/** Encoded access unit: Annex-B (start-code separated) H264 NAL bytes. */
struct EncodedAccessUnit {
  std::vector<uint8_t> annex_b;
  bool keyframe = false;
};

/** Decoded RGBA8 (non-premultiplied); caller may premultiply for GL. */
struct VideoFrameRgba {
  int width = 0;
  int height = 0;
  std::vector<uint8_t> rgba;
};

/**
 * Platform HW H264 encode/decode (V017). Best-effort: missing HW must not
 * fail call bring-up (V019). Open device / Configure* only when Camera on
 * or remote bitstream arrives.
 */
class IVideoCodec {
public:
  virtual ~IVideoCodec() = default;

  virtual std::string BackendName() const = 0;
  virtual bool HasEncoder() const = 0;
  virtual bool HasDecoder() const = 0;
  /** True when this host can open an encoder (before ConfigureEncoder). */
  virtual bool EncoderSupported() const { return true; }

  /** Target encode size/fps (a3 default ~640×360 @ 15–24). */
  virtual Roe<void> ConfigureEncoder(int width, int height, int fps) = 0;
  virtual Roe<void> ConfigureDecoder() = 0;

  virtual Roe<EncodedAccessUnit> Encode(const VideoFrameI420& frame, bool force_keyframe) = 0;
  /** Input: Annex-B access unit (possibly multiple NALs). */
  virtual Roe<VideoFrameRgba> Decode(const uint8_t* annex_b, size_t size) = 0;

  /** Optional; no-op when the backend cannot reconfigure mid-stream. */
  virtual void SetTargetBitrate(int64_t bps) { (void)bps; }

  virtual void ResetEncoder() = 0;
  virtual void ResetDecoder() = 0;
};

/** OS-selected HW codec, or unavailable stub when no backend builds. */
std::unique_ptr<IVideoCodec> CreatePlatformVideoCodec();

} // namespace pbr
