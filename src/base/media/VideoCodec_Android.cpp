#include "base/media/IVideoCodec.h"
#include "base/media/VideoCodecOs.h"
#include "base/media/VideoCodecUnavailable.h"
#include "base/media/VideoYuv.h"

#if defined(__ANDROID__)

#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <string>
#include <vector>

// V017: Android H264 encode/decode via MediaCodec (NDK).

namespace pbr {
namespace {

constexpr int64_t kMinBitrateBps = 200000;
constexpr int64_t kMaxBitrateBps = 4000000;
constexpr auto kCodecTimeoutUs = 20000LL;

int32_t EstimateBitrateBps(int width, int height, int fps) {
  const double raw = static_cast<double>(width) * static_cast<double>(height) *
                      static_cast<double>(std::max(fps, 1)) * 0.07;
  const double clamped =
      std::clamp(raw, static_cast<double>(kMinBitrateBps), static_cast<double>(kMaxBitrateBps));
  return static_cast<int32_t>(clamped);
}

struct AnnexBNal {
  const uint8_t* data = nullptr;
  size_t size = 0;
  int type = 0;
};

size_t FindStartCode(const uint8_t* data, size_t size, size_t from, size_t* start_code_len) {
  for (size_t i = from; i + 3 <= size; ++i) {
    if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
      *start_code_len = 3;
      return i;
    }
    if (i + 4 <= size && data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 0 && data[i + 3] == 1) {
      *start_code_len = 4;
      return i;
    }
  }
  *start_code_len = 0;
  return size;
}

std::vector<AnnexBNal> ParseAnnexB(const uint8_t* data, size_t size) {
  std::vector<AnnexBNal> nals;
  size_t start_code_len = 0;
  size_t pos = FindStartCode(data, size, 0, &start_code_len);
  while (pos < size) {
    const size_t nal_start = pos + start_code_len;
    size_t next_start_code_len = 0;
    const size_t next_pos = FindStartCode(data, size, nal_start, &next_start_code_len);
    if (next_pos > nal_start) {
      AnnexBNal nal;
      nal.data = data + nal_start;
      nal.size = next_pos - nal_start;
      nal.type = nal.data[0] & 0x1F;
      nals.push_back(nal);
    }
    pos = next_pos;
    start_code_len = next_start_code_len;
  }
  return nals;
}

void AppendStartCodeAndNal(std::vector<uint8_t>& out, const uint8_t* data, size_t size) {
  static const uint8_t kStartCode[4] = {0, 0, 0, 1};
  out.insert(out.end(), kStartCode, kStartCode + 4);
  out.insert(out.end(), data, data + size);
}

void I420ToNv12(const VideoFrameI420& frame, std::vector<uint8_t>& out) {
  const int width = frame.width;
  const int height = frame.height;
  const int chroma_w = (width + 1) / 2;
  const int chroma_h = (height + 1) / 2;
  out.resize(static_cast<size_t>(width) * static_cast<size_t>(height) +
             static_cast<size_t>(chroma_w) * static_cast<size_t>(chroma_h) * 2);
  uint8_t* y = out.data();
  uint8_t* uv = y + static_cast<size_t>(width) * static_cast<size_t>(height);
  for (int row = 0; row < height; ++row) {
    std::memcpy(y + static_cast<size_t>(row) * width, frame.y.data() + static_cast<size_t>(row) * width, width);
  }
  for (int row = 0; row < chroma_h; ++row) {
    const uint8_t* u_row = frame.u.data() + static_cast<size_t>(row) * chroma_w;
    const uint8_t* v_row = frame.v.data() + static_cast<size_t>(row) * chroma_w;
    uint8_t* uv_row = uv + static_cast<size_t>(row) * chroma_w * 2;
    for (int col = 0; col < chroma_w; ++col) {
      uv_row[col * 2 + 0] = u_row[col];
      uv_row[col * 2 + 1] = v_row[col];
    }
  }
}

bool Nv12ToRgba(const uint8_t* y_plane, const uint8_t* uv_plane, int width, int height, int y_stride,
                int uv_stride, VideoFrameRgba& out) {
  out.width = width;
  out.height = height;
  out.rgba.resize(static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
  for (int row = 0; row < height; ++row) {
    const uint8_t* y_row = y_plane + static_cast<size_t>(row) * y_stride;
    const uint8_t* uv_row = uv_plane + static_cast<size_t>(row / 2) * uv_stride;
    uint8_t* dst = out.rgba.data() + static_cast<size_t>(row) * width * 4;
    for (int col = 0; col < width; ++col) {
      const int y_val = static_cast<int>(y_row[col]);
      const int u_val = static_cast<int>(uv_row[(col / 2) * 2 + 0]) - 128;
      const int v_val = static_cast<int>(uv_row[(col / 2) * 2 + 1]) - 128;
      const int c = y_val - 16;
      const int r = (298 * c + 409 * v_val + 128) >> 8;
      const int g = (298 * c - 100 * u_val - 208 * v_val + 128) >> 8;
      const int b = (298 * c + 516 * u_val + 128) >> 8;
      dst[col * 4 + 0] = static_cast<uint8_t>(std::clamp(r, 0, 255));
      dst[col * 4 + 1] = static_cast<uint8_t>(std::clamp(g, 0, 255));
      dst[col * 4 + 2] = static_cast<uint8_t>(std::clamp(b, 0, 255));
      dst[col * 4 + 3] = 255;
    }
  }
  return true;
}

int PickEncoderColorFormat() {
  // NV12 — widely supported on Android HW encoders (V017).
  return 21; // COLOR_FormatYUV420SemiPlanar
}

class AndroidVideoCodec final : public IVideoCodec {
public:
  ~AndroidVideoCodec() override { ResetEncoder(); ResetDecoder(); }

  std::string BackendName() const override { return "mediacodec"; }
  bool HasEncoder() const override { return encoder_ != nullptr; }
  bool HasDecoder() const override { return decoder_configured_; }

  Roe<void> ConfigureEncoder(int width, int height, int fps) override;
  Roe<void> ConfigureDecoder() override;

  Roe<EncodedAccessUnit> Encode(const VideoFrameI420& frame, bool force_keyframe) override;
  Roe<VideoFrameRgba> Decode(const uint8_t* annex_b, size_t size) override;

  void ResetEncoder() override;
  void ResetDecoder() override;

private:
  Roe<void> EnsureDecoderConfigured(const uint8_t* sps, size_t sps_size, const uint8_t* pps, size_t pps_size);

  AMediaCodec* encoder_ = nullptr;
  int enc_width_ = 0;
  int enc_height_ = 0;
  int enc_fps_ = 0;
  int enc_color_format_ = 0;
  int64_t enc_frame_index_ = 0;

  AMediaCodec* decoder_ = nullptr;
  bool decoder_configured_ = false;
  std::vector<uint8_t> cached_sps_;
  std::vector<uint8_t> cached_pps_;
};

Roe<void> AndroidVideoCodec::ConfigureEncoder(int width, int height, int fps) {
  ResetEncoder();
  if (width <= 0 || height <= 0 || fps <= 0) {
    return Error("invalid H264 encoder configuration");
  }

  encoder_ = AMediaCodec_createEncoderByType("video/avc");
  if (!encoder_) {
    return Error("AMediaCodec_createEncoderByType failed");
  }

  enc_color_format_ = PickEncoderColorFormat();

  AMediaFormat* format = AMediaFormat_new();
  AMediaFormat_setString(format, AMEDIAFORMAT_KEY_MIME, "video/avc");
  AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_WIDTH, width);
  AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_HEIGHT, height);
  AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_BIT_RATE, EstimateBitrateBps(width, height, fps));
  AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_FRAME_RATE, fps);
  AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_I_FRAME_INTERVAL, 2);
  AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_COLOR_FORMAT, enc_color_format_);

  media_status_t status = AMediaCodec_configure(encoder_, format, nullptr, nullptr, AMEDIACODEC_CONFIGURE_FLAG_ENCODE);
  AMediaFormat_delete(format);
  if (status != AMEDIA_OK) {
    ResetEncoder();
    return Error("AMediaCodec_configure encoder failed");
  }
  if (AMediaCodec_start(encoder_) != AMEDIA_OK) {
    ResetEncoder();
    return Error("AMediaCodec_start encoder failed");
  }

  enc_width_ = width;
  enc_height_ = height;
  enc_fps_ = fps;
  enc_frame_index_ = 0;
  return {};
}

Roe<EncodedAccessUnit> AndroidVideoCodec::Encode(const VideoFrameI420& frame, bool force_keyframe) {
  if (!encoder_) {
    return Error("H264 encoder not configured");
  }
  if (frame.width != enc_width_ || frame.height != enc_height_) {
    return Error("encoder frame size mismatch");
  }

  std::vector<uint8_t> nv12;
  I420ToNv12(frame, nv12);

  ssize_t buf_index = AMediaCodec_dequeueInputBuffer(encoder_, kCodecTimeoutUs);
  if (buf_index < 0) {
    return Error("encoder input buffer unavailable");
  }

  size_t buf_size = 0;
  uint8_t* buf = AMediaCodec_getInputBuffer(encoder_, static_cast<size_t>(buf_index), &buf_size);
  if (!buf || buf_size < nv12.size()) {
    AMediaCodec_queueInputBuffer(encoder_, static_cast<size_t>(buf_index), 0, 0, 0,
                                  AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG);
    return Error("encoder input buffer too small");
  }
  std::memcpy(buf, nv12.data(), nv12.size());

  uint32_t flags = 0;
  if (force_keyframe) {
    flags |= AMEDIACODEC_BUFFER_FLAG_KEY_FRAME;
  }
  const int64_t pts_us = (enc_frame_index_ * 1000000LL) / std::max(enc_fps_, 1);
  ++enc_frame_index_;
  if (AMediaCodec_queueInputBuffer(encoder_, static_cast<size_t>(buf_index), 0, nv12.size(), pts_us, flags) !=
      AMEDIA_OK) {
    return Error("AMediaCodec_queueInputBuffer failed");
  }

  AMediaCodecBufferInfo info{};
  ssize_t out_index = AMediaCodec_dequeueOutputBuffer(encoder_, &info, kCodecTimeoutUs);
  while (out_index == AMEDIACODEC_INFO_TRY_AGAIN_LATER) {
    out_index = AMediaCodec_dequeueOutputBuffer(encoder_, &info, kCodecTimeoutUs);
  }
  if (out_index < 0) {
    return Error("encoder produced no output");
  }

  size_t out_size = 0;
  uint8_t* out_buf = AMediaCodec_getOutputBuffer(encoder_, static_cast<size_t>(out_index), &out_size);
  EncodedAccessUnit unit;
  if (out_buf && info.size > 0) {
    unit.annex_b.assign(out_buf + info.offset, out_buf + info.offset + info.size);
    unit.keyframe = (info.flags & AMEDIACODEC_BUFFER_FLAG_KEY_FRAME) != 0;
    // MediaCodec emits AVCC length-prefixed NALs; convert to Annex-B for libdatachannel.
    if (!unit.annex_b.empty() && unit.annex_b[0] != 0) {
      std::vector<uint8_t> annex_b;
      size_t offset = 0;
      while (offset + 4 <= unit.annex_b.size()) {
        uint32_t nal_size = (static_cast<uint32_t>(unit.annex_b[offset]) << 24) |
                            (static_cast<uint32_t>(unit.annex_b[offset + 1]) << 16) |
                            (static_cast<uint32_t>(unit.annex_b[offset + 2]) << 8) |
                            static_cast<uint32_t>(unit.annex_b[offset + 3]);
        offset += 4;
        if (offset + nal_size > unit.annex_b.size()) {
          break;
        }
        AppendStartCodeAndNal(annex_b, unit.annex_b.data() + offset, nal_size);
        offset += nal_size;
      }
      if (!annex_b.empty()) {
        unit.annex_b = std::move(annex_b);
      }
    }
  }
  AMediaCodec_releaseOutputBuffer(encoder_, static_cast<size_t>(out_index), false);
  if (unit.annex_b.empty()) {
    return Error("empty H264 encoder output");
  }
  return unit;
}

Roe<void> AndroidVideoCodec::ConfigureDecoder() {
  ResetDecoder();
  decoder_configured_ = true;
  return {};
}

Roe<void> AndroidVideoCodec::EnsureDecoderConfigured(const uint8_t* sps, size_t sps_size, const uint8_t* pps,
                                                      size_t pps_size) {
  const bool unchanged = decoder_ && cached_sps_.size() == sps_size && cached_pps_.size() == pps_size &&
                          std::equal(cached_sps_.begin(), cached_sps_.end(), sps) &&
                          std::equal(cached_pps_.begin(), cached_pps_.end(), pps);
  if (unchanged) {
    return {};
  }

  if (decoder_) {
    AMediaCodec_stop(decoder_);
    AMediaCodec_delete(decoder_);
    decoder_ = nullptr;
  }

  decoder_ = AMediaCodec_createDecoderByType("video/avc");
  if (!decoder_) {
    return Error("AMediaCodec_createDecoderByType failed");
  }

  std::vector<uint8_t> csd;
  csd.push_back(0);
  csd.push_back(0);
  csd.push_back(0);
  csd.push_back(1);
  csd.insert(csd.end(), sps, sps + sps_size);
  csd.push_back(0);
  csd.push_back(0);
  csd.push_back(0);
  csd.push_back(1);
  csd.insert(csd.end(), pps, pps + pps_size);

  AMediaFormat* format = AMediaFormat_new();
  AMediaFormat_setString(format, AMEDIAFORMAT_KEY_MIME, "video/avc");
  AMediaFormat_setBuffer(format, "csd-0", csd.data(), csd.size());

  if (AMediaCodec_configure(decoder_, format, nullptr, nullptr, 0) != AMEDIA_OK) {
    AMediaFormat_delete(format);
    return Error("AMediaCodec_configure decoder failed");
  }
  AMediaFormat_delete(format);
  if (AMediaCodec_start(decoder_) != AMEDIA_OK) {
    return Error("AMediaCodec_start decoder failed");
  }

  cached_sps_.assign(sps, sps + sps_size);
  cached_pps_.assign(pps, pps + pps_size);
  return {};
}

Roe<VideoFrameRgba> AndroidVideoCodec::Decode(const uint8_t* annex_b, size_t size) {
  if (!decoder_configured_) {
    return Error("H264 decoder not configured");
  }
  if (!annex_b || size == 0) {
    return Error("empty H264 decoder input");
  }

  const std::vector<AnnexBNal> nals = ParseAnnexB(annex_b, size);
  if (nals.empty()) {
    return Error("no NAL units found");
  }

  const uint8_t* sps = nullptr;
  size_t sps_size = 0;
  const uint8_t* pps = nullptr;
  size_t pps_size = 0;
  std::vector<uint8_t> avcc;
  bool has_slice = false;

  for (const auto& nal : nals) {
    if (nal.type == 7) {
      sps = nal.data;
      sps_size = nal.size;
      continue;
    }
    if (nal.type == 8) {
      pps = nal.data;
      pps_size = nal.size;
      continue;
    }
    has_slice = true;
    const uint32_t nal_size = static_cast<uint32_t>(nal.size);
    avcc.push_back(static_cast<uint8_t>((nal_size >> 24) & 0xFF));
    avcc.push_back(static_cast<uint8_t>((nal_size >> 16) & 0xFF));
    avcc.push_back(static_cast<uint8_t>((nal_size >> 8) & 0xFF));
    avcc.push_back(static_cast<uint8_t>(nal_size & 0xFF));
    avcc.insert(avcc.end(), nal.data, nal.data + nal.size);
  }

  if (sps && pps) {
    if (auto cfg = EnsureDecoderConfigured(sps, sps_size, pps, pps_size); !cfg) {
      return cfg.error();
    }
  }
  if (!decoder_) {
    return Error("H264 decoder waiting for SPS/PPS");
  }
  if (!has_slice || avcc.empty()) {
    return Error("access unit contained no slice data");
  }

  ssize_t buf_index = AMediaCodec_dequeueInputBuffer(decoder_, kCodecTimeoutUs);
  if (buf_index < 0) {
    return Error("decoder input buffer unavailable");
  }
  size_t buf_size = 0;
  uint8_t* buf = AMediaCodec_getInputBuffer(decoder_, static_cast<size_t>(buf_index), &buf_size);
  if (!buf || buf_size < avcc.size()) {
    return Error("decoder input buffer too small");
  }
  std::memcpy(buf, avcc.data(), avcc.size());
  if (AMediaCodec_queueInputBuffer(decoder_, static_cast<size_t>(buf_index), 0, avcc.size(), 0, 0) != AMEDIA_OK) {
    return Error("decoder queueInputBuffer failed");
  }

  AMediaCodecBufferInfo info{};
  ssize_t out_index = AMediaCodec_dequeueOutputBuffer(decoder_, &info, kCodecTimeoutUs);
  while (out_index == AMEDIACODEC_INFO_TRY_AGAIN_LATER) {
    out_index = AMediaCodec_dequeueOutputBuffer(decoder_, &info, kCodecTimeoutUs);
  }
  if (out_index < 0) {
    return Error("decoder produced no output");
  }

  VideoFrameRgba result;
  AMediaFormat* out_format = AMediaCodec_getOutputFormat(decoder_);
  int width = 0;
  int height = 0;
  if (out_format) {
    AMediaFormat_getInt32(out_format, AMEDIAFORMAT_KEY_WIDTH, &width);
    AMediaFormat_getInt32(out_format, AMEDIAFORMAT_KEY_HEIGHT, &height);
    AMediaFormat_delete(out_format);
  }

  size_t out_size = 0;
  uint8_t* out_buf = AMediaCodec_getOutputBuffer(decoder_, static_cast<size_t>(out_index), &out_size);
  if (out_buf && info.size > 0 && width > 0 && height > 0) {
    const uint8_t* y = out_buf + info.offset;
    const uint8_t* uv = y + static_cast<size_t>(width) * static_cast<size_t>(height);
    if (!Nv12ToRgba(y, uv, width, height, width, width, result)) {
      AMediaCodec_releaseOutputBuffer(decoder_, static_cast<size_t>(out_index), false);
      return Error("failed to convert decoded frame to RGBA");
    }
  } else {
    AMediaCodec_releaseOutputBuffer(decoder_, static_cast<size_t>(out_index), false);
    return Error("decoder output empty");
  }
  AMediaCodec_releaseOutputBuffer(decoder_, static_cast<size_t>(out_index), false);
  return result;
}

void AndroidVideoCodec::ResetEncoder() {
  if (encoder_) {
    AMediaCodec_stop(encoder_);
    AMediaCodec_delete(encoder_);
    encoder_ = nullptr;
  }
  enc_width_ = 0;
  enc_height_ = 0;
  enc_fps_ = 0;
  enc_color_format_ = 0;
  enc_frame_index_ = 0;
}

void AndroidVideoCodec::ResetDecoder() {
  if (decoder_) {
    AMediaCodec_stop(decoder_);
    AMediaCodec_delete(decoder_);
    decoder_ = nullptr;
  }
  cached_sps_.clear();
  cached_pps_.clear();
  decoder_configured_ = false;
}

} // namespace

std::unique_ptr<IVideoCodec> CreateAndroidVideoCodec() {
  AMediaCodec* probe = AMediaCodec_createEncoderByType("video/avc");
  if (!probe) {
    return MakeUnavailableVideoCodec("MediaCodec H264 encode unavailable on this device");
  }
  AMediaCodec_delete(probe);
  return std::make_unique<AndroidVideoCodec>();
}

} // namespace pbr

#endif // __ANDROID__
