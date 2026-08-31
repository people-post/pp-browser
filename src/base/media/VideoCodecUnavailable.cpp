#include "base/media/VideoCodecUnavailable.h"
#include "common/PbrCompat.h"

namespace pbr {
namespace {

class UnavailableVideoCodec final : public IVideoCodec {
public:
  explicit UnavailableVideoCodec(std::string reason) : reason_(std::move(reason)) {}

  std::string BackendName() const override { return "unavailable"; }
  bool HasEncoder() const override { return false; }
  bool HasDecoder() const override { return false; }
  bool EncoderSupported() const override { return false; }

  Roe<void> ConfigureEncoder(int, int, int) override {
    return Error("H264 encoder unavailable: " + reason_);
  }
  Roe<void> ConfigureDecoder() override { return Error("H264 decoder unavailable: " + reason_); }

  Roe<EncodedAccessUnit> Encode(const VideoFrameI420&, bool) override {
    return Error("H264 encoder unavailable: " + reason_);
  }
  Roe<VideoFrameRgba> Decode(const uint8_t*, size_t) override {
    return Error("H264 decoder unavailable: " + reason_);
  }

  void ResetEncoder() override {}
  void ResetDecoder() override {}

private:
  std::string reason_;
};

} // namespace

std::unique_ptr<IVideoCodec> MakeUnavailableVideoCodec(std::string reason) {
  return std::make_unique<UnavailableVideoCodec>(std::move(reason));
}

} // namespace pbr
