#include "lib/media/SyntheticVideo.h"

#include <algorithm>
#include <utility>
#include "common/PbrCompat.h"

namespace pbr {
namespace {

class SyntheticCodec final : public IVideoCodec {
public:
  std::string BackendName() const override { return "synthetic"; }
  bool HasEncoder() const override { return encoder_; }
  bool HasDecoder() const override { return decoder_; }

  Roe<void> ConfigureEncoder(int /*width*/, int /*height*/, const int fps) override {
    fps_ = std::max(fps, 1);
    frame_ = 0;
    encoder_ = true;
    return {};
  }
  Roe<void> ConfigureDecoder() override {
    decoder_ = true;
    return {};
  }

  Roe<EncodedAccessUnit> Encode(const VideoFrameI420& /*frame*/, const bool force_keyframe) override {
    if (!encoder_) {
      return Error("synthetic encoder not configured");
    }
    const bool keyframe = force_keyframe || frame_ % static_cast<uint64_t>(fps_ * 2) == 0;
    const size_t bytes =
        keyframe ? SyntheticVideo::kKeyframeBytes : static_cast<size_t>(std::max<int64_t>(200, target_bps_ / fps_ / 8));
    EncodedAccessUnit unit;
    unit.keyframe = keyframe;
    unit.annex_b.resize(bytes);
    // An Annex-B start code and an IDR / non-IDR slice header byte, then filler that does not compress.
    unit.annex_b[0] = 0;
    unit.annex_b[1] = 0;
    unit.annex_b[2] = 0;
    unit.annex_b[3] = 1;
    unit.annex_b[4] = keyframe ? 0x65 : 0x41;
    uint32_t x = static_cast<uint32_t>(frame_ * 2654435761u + 1);
    for (size_t i = 5; i < bytes; ++i) {
      x ^= x << 13;
      x ^= x >> 17;
      x ^= x << 5;
      unit.annex_b[i] = static_cast<uint8_t>(x);
    }
    ++frame_;
    return unit;
  }

  Roe<std::optional<VideoFrameRgba>> Decode(const uint8_t* annex_b, const size_t size) override {
    if (!decoder_) {
      return Error("synthetic decoder not configured");
    }
    if (size < 5 || annex_b[0] != 0 || annex_b[1] != 0 || annex_b[2] != 0 || annex_b[3] != 1) {
      return Error("not an Annex-B access unit");
    }
    VideoFrameRgba frame;
    frame.width = 16;
    frame.height = 16;
    frame.rgba.assign(static_cast<size_t>(16 * 16 * 4), 0x80);
    return std::optional<VideoFrameRgba>(std::move(frame));
  }

  void SetTargetBitrate(const int64_t bps) override { target_bps_ = bps > 0 ? bps : target_bps_; }
  void ResetEncoder() override { encoder_ = false; }
  void ResetDecoder() override { decoder_ = false; }

private:
  bool encoder_ = false;
  bool decoder_ = false;
  int fps_ = 20;
  uint64_t frame_ = 0;
  int64_t target_bps_ = SyntheticVideo::kDefaultTargetBps;
};

class SyntheticCamera final : public ICameraEndpoint {
public:
  CameraGeometry Geometry() const override {
    CameraGeometry geometry;
    geometry.encode_width = 640;
    geometry.encode_height = 360;
    geometry.front_facing = true;
    return geometry;
  }
  std::optional<VideoFrameRgba> NextFrame() override {
    VideoFrameRgba frame;
    frame.width = 640;
    frame.height = 360;
    frame.rgba.assign(static_cast<size_t>(640 * 360 * 4), static_cast<uint8_t>(++tick_));
    return frame;
  }

private:
  uint8_t tick_ = 0;
};

class SyntheticCameraBackend final : public IMediaDeviceBackend {
public:
  explicit SyntheticCameraBackend(std::unique_ptr<IMediaDeviceBackend> inner) : inner_(std::move(inner)) {}

  std::unique_ptr<IAudioEndpoint> OpenAudio(const MediaDeviceKind kind, const AudioDeviceFormat& format,
                                            const std::function<bool()>& still_wanted, std::string* error) override {
    return inner_->OpenAudio(kind, format, still_wanted, error);
  }
  std::unique_ptr<ICameraEndpoint> OpenCamera(const CameraRequestFormat& /*format*/, std::string* /*error*/) override {
    return std::make_unique<SyntheticCamera>();
  }
  VoiceDuplexEndpoints OpenVoiceDuplex(const AudioDeviceFormat& format, std::string* error) override {
    return inner_->OpenVoiceDuplex(format, error);
  }
  std::chrono::milliseconds ReopenSettle(const MediaDeviceKind kind) const override {
    return inner_->ReopenSettle(kind);
  }

private:
  std::unique_ptr<IMediaDeviceBackend> inner_;
};

} // namespace

std::unique_ptr<IVideoCodec> SyntheticVideo::CreateCodec() {
  return std::make_unique<SyntheticCodec>();
}

std::unique_ptr<IMediaDeviceBackend> SyntheticVideo::WithCamera(std::unique_ptr<IMediaDeviceBackend> inner) {
  return std::make_unique<SyntheticCameraBackend>(std::move(inner));
}

} // namespace pbr
