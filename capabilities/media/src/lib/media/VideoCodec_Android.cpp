#include "lib/media/DecodeStallPolicy.h"
#include "lib/media/IVideoCodec.h"
#include "lib/media/VideoCodecOs.h"
#include "lib/media/VideoYuv.h"

#if defined(__ANDROID__)

#include <android/api-level.h>
#include <dlfcn.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <optional>
#include <string>
#include <vector>
#include "common/Logger.h"
#include "common/PbrCompat.h"

// V017: Android H264 encode/decode via MediaCodec (NDK).

namespace pbr {
namespace {

constexpr int64_t kMinBitrateBps = 200000;
constexpr int64_t kMaxBitrateBps = 4000000;
constexpr auto kCodecTimeoutUs = 20000LL;
/** Wait for a decoder input buffer (under the media lock — B125: was 50 ms, plus up to 600 ms for output). */
constexpr auto kDecodeInputTimeoutUs = 10000LL;
/** The one retry after draining outputs waits longer (≤ 40 ms in all) before the frame is dropped. */
constexpr auto kDecodeInputRetryTimeoutUs = 30000LL;
/** Frames in a row that never reached the decoder (no input buffer, then dropped until an IDR) count
 *  as a stall: a decoder that takes no input never produces output either. */
constexpr int kInputStarvedStallFrames = 30;
/** Output buffers taken per Decode at most (they are released as they come; only the last is shown). */
constexpr int kMaxDrainedOutputs = 32;
/** Device API level from which the decoder is asked for low-latency output (MediaFormat "low-latency"). */
constexpr int kLowLatencyApiLevel = 30;
/**
 * Off until proven on devices: a vivo's calls owner hung in the decoder's teardown on the first video
 * call with low-latency on (B128, #452). The code and its fallback stay; flip to try it again.
 */
constexpr bool kTryLowLatencyDecoder = false;
/** At most one "input buffer unavailable" warning this often. */
constexpr int64_t kInputDropWarnEveryMs = 2000;
/** A Decode slower than this (codec configure not counted) is logged — it runs under the media lock. */
constexpr int64_t kSlowDecodeMs = 50;
/** Per decoder (one per remote stream per call): slow-decode, stall and input-drop warnings each. */
constexpr int kMaxDecoderWarns = 10;
constexpr int kColorFormatYv12 = 19; // COLOR_FormatYUV420Planar
constexpr int kColorFormatNv12 = 21; // COLOR_FormatYUV420SemiPlanar

int32_t EstimateBitrateBps(int width, int height, int fps) {
  const double raw =
      static_cast<double>(width) * static_cast<double>(height) * static_cast<double>(std::max(fps, 1)) * 0.07;
  const double clamped = std::clamp(raw, static_cast<double>(kMinBitrateBps), static_cast<double>(kMaxBitrateBps));
  return static_cast<int32_t>(clamped);
}

logging::Logger& DecoderLog() {
  static logging::Logger logger = logging::getLogger("MediaEngine.AndroidDecoder");
  return logger;
}

int64_t SteadyMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

/**
 * The codec component's name (e.g. c2.qti.avc.decoder). AMediaCodec_getName is API 28 and this
 * builds for API 24, so it is looked up at run time; "unknown" before 28.
 */
std::string CodecName(AMediaCodec* codec) {
  using GetNameFn = media_status_t (*)(AMediaCodec*, char**);
  using ReleaseNameFn = void (*)(AMediaCodec*, char*);
  static void* const lib = dlopen("libmediandk.so", RTLD_NOW);
  static const auto get_name = lib ? reinterpret_cast<GetNameFn>(dlsym(lib, "AMediaCodec_getName")) : nullptr;
  static const auto release_name =
      lib ? reinterpret_cast<ReleaseNameFn>(dlsym(lib, "AMediaCodec_releaseName")) : nullptr;
  char* name = nullptr;
  if (!get_name || !release_name || get_name(codec, &name) != AMEDIA_OK || !name) {
    return "unknown";
  }
  std::string out(name);
  release_name(codec, name);
  return out;
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

// Minimal Exp-Golomb bit reader for SPS dimension extraction.
class BitReader {
public:
  BitReader(const uint8_t* data, size_t size) : data_(data), size_(size) {}

  bool Ok() const { return !error_; }

  uint32_t ReadBits(int n) {
    uint32_t v = 0;
    for (int i = 0; i < n; ++i) {
      if (bit_pos_ / 8 >= size_) {
        error_ = true;
        return 0;
      }
      const uint8_t byte = data_[bit_pos_ / 8];
      const int bit = 7 - static_cast<int>(bit_pos_ % 8);
      v = (v << 1) | static_cast<uint32_t>((byte >> bit) & 1);
      ++bit_pos_;
    }
    return v;
  }

  uint32_t ReadUe() {
    int zeros = 0;
    while (Ok() && ReadBits(1) == 0) {
      ++zeros;
      if (zeros > 31) {
        error_ = true;
        return 0;
      }
    }
    if (!Ok()) {
      return 0;
    }
    if (zeros == 0) {
      return 0;
    }
    return (1u << zeros) - 1u + ReadBits(zeros);
  }

  int32_t ReadSe() {
    const uint32_t v = ReadUe();
    if (!Ok()) {
      return 0;
    }
    const int32_t n = static_cast<int32_t>(v);
    return (n % 2 == 0) ? -(n / 2) : ((n + 1) / 2);
  }

  void SkipBits(int n) { (void)ReadBits(n); }

private:
  const uint8_t* data_ = nullptr;
  size_t size_ = 0;
  size_t bit_pos_ = 0;
  bool error_ = false;
};

std::vector<uint8_t> StripEmulationPrevention(const uint8_t* data, size_t size) {
  std::vector<uint8_t> out;
  out.reserve(size);
  for (size_t i = 0; i < size; ++i) {
    if (i + 2 < size && data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 3) {
      out.push_back(0);
      out.push_back(0);
      i += 2;
      continue;
    }
    out.push_back(data[i]);
  }
  return out;
}

// Qualcomm OMX.qcom.video.decoder.avc rejects configure without width/height
// (ACodec configureCodec → -38 / OMX_ErrorUndefined). Parse from SPS.
bool ParseSpsDimensions(const uint8_t* sps_nal, size_t sps_size, int& width, int& height) {
  if (!sps_nal || sps_size < 4) {
    return false;
  }
  const std::vector<uint8_t> rbsp = StripEmulationPrevention(sps_nal, sps_size);
  if (rbsp.size() < 4) {
    return false;
  }
  BitReader br(rbsp.data() + 1, rbsp.size() - 1); // skip NAL header
  const int profile_idc = static_cast<int>(br.ReadBits(8));
  br.SkipBits(8);       // constraint flags
  (void)br.ReadBits(8); // level_idc
  (void)br.ReadUe();    // sps id
  int chroma_format_idc = 1;
  if (profile_idc == 100 || profile_idc == 110 || profile_idc == 122 || profile_idc == 244 || profile_idc == 44 ||
      profile_idc == 83 || profile_idc == 86 || profile_idc == 118 || profile_idc == 128 || profile_idc == 138 ||
      profile_idc == 139 || profile_idc == 134) {
    chroma_format_idc = static_cast<int>(br.ReadUe());
    if (chroma_format_idc == 3) {
      br.SkipBits(1);
    }
    (void)br.ReadUe(); // bit_depth_luma
    (void)br.ReadUe(); // bit_depth_chroma
    br.SkipBits(1);    // qpprime_y_zero_transform_bypass
    if (br.ReadBits(1)) {
      return false; // scaling matrix unsupported here
    }
  }
  (void)br.ReadUe(); // log2_max_frame_num
  const int poc_type = static_cast<int>(br.ReadUe());
  if (poc_type == 0) {
    (void)br.ReadUe();
  } else if (poc_type == 1) {
    br.SkipBits(1);
    (void)br.ReadSe();
    (void)br.ReadSe();
    const uint32_t n = br.ReadUe();
    // num_ref_frames_in_pic_order_cnt_cycle is a peer-controlled ue(v) (up to ~4 billion); the
    // spec caps it at 255, and the bitstream cannot possibly contain that many entries anyway —
    // bound it so a hostile SPS can't force a multi-billion-iteration loop.
    if (n > 255) {
      return false;
    }
    for (uint32_t i = 0; i < n; ++i) {
      (void)br.ReadSe();
    }
  }
  (void)br.ReadUe(); // max_num_ref_frames
  br.SkipBits(1);    // gaps_in_frame_num
  const int pic_width_in_mbs_minus1 = static_cast<int>(br.ReadUe());
  const int pic_height_in_map_units_minus1 = static_cast<int>(br.ReadUe());
  const int frame_mbs_only_flag = static_cast<int>(br.ReadBits(1));
  if (!frame_mbs_only_flag) {
    br.SkipBits(1);
  }
  br.SkipBits(1); // direct_8x8_inference
  const int frame_cropping_flag = static_cast<int>(br.ReadBits(1));
  int crop_left = 0;
  int crop_right = 0;
  int crop_top = 0;
  int crop_bottom = 0;
  if (frame_cropping_flag) {
    crop_left = static_cast<int>(br.ReadUe());
    crop_right = static_cast<int>(br.ReadUe());
    crop_top = static_cast<int>(br.ReadUe());
    crop_bottom = static_cast<int>(br.ReadUe());
  }
  if (!br.Ok()) {
    return false;
  }
  // pic_width_in_mbs_minus1 / pic_height_in_map_units_minus1 / crop_* are peer-controlled ue(v)
  // values (up to ~4 billion each); do the whole computation in int64_t and bound the result to
  // a generous real-world resolution ceiling, so a hostile SPS can't overflow the `int` math
  // below into a bogus (or negative-then-cast-huge) width/height.
  constexpr int64_t kMaxReasonableDimension = 16384;
  const int64_t width_mbs = static_cast<int64_t>(pic_width_in_mbs_minus1) + 1;
  const int64_t height_mbs = (static_cast<int64_t>(pic_height_in_map_units_minus1) + 1) * (2 - frame_mbs_only_flag);
  int64_t wide = width_mbs * 16;
  int64_t tall = height_mbs * 16;
  if (wide <= 0 || tall <= 0 || wide > kMaxReasonableDimension || tall > kMaxReasonableDimension) {
    return false;
  }
  if (frame_cropping_flag) {
    const int64_t crop_unit_x = (chroma_format_idc == 1 || chroma_format_idc == 2) ? 2 : 1;
    const int64_t crop_unit_y = (chroma_format_idc == 1) ? (2 * (2 - frame_mbs_only_flag)) : (2 - frame_mbs_only_flag);
    wide -= (static_cast<int64_t>(crop_left) + crop_right) * crop_unit_x;
    tall -= (static_cast<int64_t>(crop_top) + crop_bottom) * crop_unit_y;
  }
  if (wide <= 0 || tall <= 0 || wide > kMaxReasonableDimension || tall > kMaxReasonableDimension) {
    return false;
  }
  width = static_cast<int>(wide);
  height = static_cast<int>(tall);
  return true;
}

std::vector<uint8_t> ToAnnexB(const uint8_t* data, size_t size) {
  std::vector<uint8_t> annex_b;
  if (!data || size == 0) {
    return annex_b;
  }
  // Already start-code prefixed.
  if (size >= 3 && data[0] == 0 && data[1] == 0 && (data[2] == 1 || (size >= 4 && data[2] == 0 && data[3] == 1))) {
    annex_b.assign(data, data + size);
    return annex_b;
  }
  size_t offset = 0;
  while (offset + 4 <= size) {
    const uint32_t nal_size = (static_cast<uint32_t>(data[offset]) << 24) |
                              (static_cast<uint32_t>(data[offset + 1]) << 16) |
                              (static_cast<uint32_t>(data[offset + 2]) << 8) | static_cast<uint32_t>(data[offset + 3]);
    offset += 4;
    if (nal_size == 0 || offset + nal_size > size) {
      break;
    }
    AppendStartCodeAndNal(annex_b, data + offset, nal_size);
    offset += nal_size;
  }
  return annex_b;
}

// Drain MediaCodec output, ignoring format/buffer-changed notices that otherwise
// abort the first remote frames on many devices.
ssize_t DequeueOutputBuffer(AMediaCodec* codec, AMediaCodecBufferInfo* info, int max_attempts,
                            int64_t timeout_us = kCodecTimeoutUs) {
  for (int attempt = 0; attempt < max_attempts; ++attempt) {
    const ssize_t index = AMediaCodec_dequeueOutputBuffer(codec, info, timeout_us);
    if (index == AMEDIACODEC_INFO_TRY_AGAIN_LATER) {
      continue;
    }
    if (index == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED || index == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) {
      continue;
    }
    return index;
  }
  return AMEDIACODEC_INFO_TRY_AGAIN_LATER;
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

bool Nv12ToRgba(const uint8_t* y_plane, const uint8_t* uv_plane, int width, int height, int y_stride, int uv_stride,
                VideoFrameRgba& out) {
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

bool I420PlanesToRgba(const uint8_t* y_plane, const uint8_t* u_plane, const uint8_t* v_plane, int width, int height,
                      int y_stride, int u_stride, int v_stride, VideoFrameRgba& out) {
  VideoFrameI420 i420;
  i420.width = width;
  i420.height = height;
  i420.y.resize(static_cast<size_t>(width) * static_cast<size_t>(height));
  i420.u.resize(static_cast<size_t>((width + 1) / 2) * static_cast<size_t>((height + 1) / 2));
  i420.v.resize(i420.u.size());
  for (int row = 0; row < height; ++row) {
    std::memcpy(i420.y.data() + static_cast<size_t>(row) * width, y_plane + static_cast<size_t>(row) * y_stride,
                static_cast<size_t>(width));
  }
  const int chroma_h = (height + 1) / 2;
  const int chroma_w = (width + 1) / 2;
  for (int row = 0; row < chroma_h; ++row) {
    std::memcpy(i420.u.data() + static_cast<size_t>(row) * chroma_w, u_plane + static_cast<size_t>(row) * u_stride,
                static_cast<size_t>(chroma_w));
    std::memcpy(i420.v.data() + static_cast<size_t>(row) * chroma_w, v_plane + static_cast<size_t>(row) * v_stride,
                static_cast<size_t>(chroma_w));
  }
  return I420ToRgba(i420, out);
}

int PickEncoderColorFormat() {
  // NV12 — widely supported on Android HW encoders (V017).
  return 21; // COLOR_FormatYUV420SemiPlanar
}

/** Whether this phone has an H.264 encoder: probed once per process (a decoder never needs one). */
bool H264EncoderPresent() {
  static const bool present = [] {
    AMediaCodec* probe = AMediaCodec_createEncoderByType("video/avc");
    if (!probe) {
      return false;
    }
    AMediaCodec_delete(probe);
    return true;
  }();
  return present;
}

class AndroidVideoCodec final : public IVideoCodec {
public:
  ~AndroidVideoCodec() override {
    ResetEncoder();
    ResetDecoder();
  }

  std::string BackendName() const override { return "mediacodec"; }
  bool EncoderSupported() const override { return H264EncoderPresent(); }
  bool HasEncoder() const override { return encoder_ != nullptr; }
  bool HasDecoder() const override { return decoder_configured_; }

  Roe<void> ConfigureEncoder(int width, int height, int fps) override;
  Roe<void> ConfigureDecoder() override;

  Roe<EncodedAccessUnit> Encode(const VideoFrameI420& frame, bool force_keyframe) override;
  Roe<std::optional<VideoFrameRgba>> Decode(const uint8_t* annex_b, size_t size) override;

  void ResetEncoder() override;
  void ResetDecoder() override;

private:
  Roe<void> EnsureDecoderConfigured(const uint8_t* sps, size_t sps_size, const uint8_t* pps, size_t pps_size);
  Roe<std::optional<VideoFrameRgba>> DecodeAccessUnit(const uint8_t* annex_b, size_t size);
  /** Takes every ready output buffer without waiting; keeps the newest in held_output_, releases the rest. */
  void DrainOutputs();
  void ReleaseHeldOutput();
  /** Converts held_output_ to RGBA and releases it. */
  Roe<VideoFrameRgba> ConvertHeldOutput();

  AMediaCodec* encoder_ = nullptr;
  int enc_width_ = 0;
  int enc_height_ = 0;
  int enc_fps_ = 0;
  int enc_color_format_ = 0;
  int64_t enc_frame_index_ = 0;
  std::vector<uint8_t> pending_param_sets_;

  AMediaCodec* decoder_ = nullptr;
  bool decoder_configured_ = false;
  std::vector<uint8_t> cached_sps_;
  std::vector<uint8_t> cached_pps_;
  int64_t dec_frame_index_ = 0;
  int dec_color_format_ = kColorFormatNv12;
  /** Newest output buffer taken by DrainOutputs and not yet converted (index < 0: none). */
  ssize_t held_output_index_ = -1;
  AMediaCodecBufferInfo held_output_info_{};
  /** A frame was dropped (no input buffer) or the codec rebuilt: frames are dropped until an IDR. */
  IdrWait idr_wait_;
  /** Frames not queued since an input buffer was last unavailable (0 = input is flowing). */
  int input_starved_frames_ = 0;
  DecodeStallPolicy stall_;
  int input_drops_ = 0;
  int64_t last_input_drop_warn_ms_ = 0;
  /** Diagnostics (B125): the first output of each configured codec, slow decodes. */
  int64_t configured_at_ms_ = 0;
  int queued_since_configure_ = 0;
  bool first_output_logged_ = false;
  int dec_stride_ = 0;
  int slow_decode_warns_ = 0;
  int stall_warns_ = 0;
  int input_drop_warns_ = 0;
  /** Time spent creating / configuring the codec in the current Decode (not a slow decode). */
  int64_t configure_ms_in_decode_ = 0;
  /** The last configure: how long it took and whether the decoder took low-latency. */
  int64_t configure_ms_ = 0;
  bool low_latency_ = false;
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
  AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_I_FRAME_INTERVAL, kKeyframeIntervalSeconds);
  AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_COLOR_FORMAT, enc_color_format_);
  // Constrained-Baseline-friendly profile for Win/macOS/Linux HW decoders (V017).
  // AMEDIAFORMAT_KEY_PROFILE / LEVEL exist from API 28; string keys are identical.
#if __ANDROID_API__ >= 28
  AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_PROFILE, 1); // AVCProfileBaseline
  AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_LEVEL, 512); // AVCLevel3_1
#else
  AMediaFormat_setInt32(format, "profile", 1); // AVCProfileBaseline
  AMediaFormat_setInt32(format, "level", 512); // AVCLevel3_1
#endif

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
    AMediaCodec_queueInputBuffer(encoder_, static_cast<size_t>(buf_index), 0, 0, 0, 0);
    return Error("encoder input buffer too small");
  }
  std::memcpy(buf, nv12.data(), nv12.size());

  if (force_keyframe) {
#if __ANDROID_API__ >= 26
    AMediaFormat* params = AMediaFormat_new();
    AMediaFormat_setInt32(params, "request-sync", 1);
    AMediaCodec_setParameters(encoder_, params);
    AMediaFormat_delete(params);
#endif
  }
  const int64_t pts_us = (enc_frame_index_ * 1000000LL) / std::max(enc_fps_, 1);
  ++enc_frame_index_;
  if (AMediaCodec_queueInputBuffer(encoder_, static_cast<size_t>(buf_index), 0, nv12.size(), pts_us, 0) != AMEDIA_OK) {
    return Error("AMediaCodec_queueInputBuffer failed");
  }

  EncodedAccessUnit unit;
  // Drain config buffers (SPS/PPS) then the encoded access unit. Config often
  // arrives as a separate output before the first IDR on MediaCodec.
  for (int attempt = 0; attempt < 10 && unit.annex_b.empty(); ++attempt) {
    AMediaCodecBufferInfo info{};
    const ssize_t out_index = DequeueOutputBuffer(encoder_, &info, 4);
    if (out_index < 0) {
      break;
    }
    size_t out_size = 0;
    uint8_t* out_buf = AMediaCodec_getOutputBuffer(encoder_, static_cast<size_t>(out_index), &out_size);
    if (out_buf && info.size > 0) {
      std::vector<uint8_t> chunk = ToAnnexB(out_buf + info.offset, static_cast<size_t>(info.size));
      if ((info.flags & AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG) != 0) {
        pending_param_sets_ = std::move(chunk);
      } else if (!chunk.empty()) {
        unit.keyframe = (info.flags & AMEDIACODEC_BUFFER_FLAG_KEY_FRAME) != 0;
        if (unit.keyframe || force_keyframe) {
          unit.keyframe = true;
          if (!pending_param_sets_.empty()) {
            const auto nals = ParseAnnexB(chunk.data(), chunk.size());
            bool has_sps = false;
            for (const auto& nal : nals) {
              if (nal.type == 7) {
                has_sps = true;
                break;
              }
            }
            if (!has_sps) {
              unit.annex_b = pending_param_sets_;
            }
          }
        }
        unit.annex_b.insert(unit.annex_b.end(), chunk.begin(), chunk.end());
      }
    }
    AMediaCodec_releaseOutputBuffer(encoder_, static_cast<size_t>(out_index), false);
  }
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

  auto fail_decoder = [this](const char* message) -> Roe<void> {
    if (decoder_) {
      ReleaseHeldOutput();
      AMediaCodec_stop(decoder_);
      AMediaCodec_delete(decoder_);
      decoder_ = nullptr;
    }
    return Error(message);
  };

  if (decoder_) {
    ReleaseHeldOutput();
    AMediaCodec_stop(decoder_);
    AMediaCodec_delete(decoder_);
    decoder_ = nullptr;
  }

  int width = 0;
  int height = 0;
  if (!ParseSpsDimensions(sps, sps_size, width, height)) {
    return Error("failed to parse H264 SPS dimensions for MediaCodec");
  }

  decoder_ = AMediaCodec_createDecoderByType("video/avc");
  if (!decoder_) {
    return Error("AMediaCodec_createDecoderByType failed");
  }

  // Byte-stream (Annex-B) CSD: separate SPS / PPS. Input buffers must also be
  // Annex-B — mixing AVCC length-prefixed NALs with Annex-B CSD fails on many
  // devices and is the Win→Android remote-black failure mode.
  //
  // Qualcomm OMX also requires width/height at configure time; CSD alone is not
  // enough (configureCodec → -38 / 0x80001001). Win MF SPS is 640×360 Baseline.
  std::vector<uint8_t> csd0;
  AppendStartCodeAndNal(csd0, sps, sps_size);
  std::vector<uint8_t> csd1;
  AppendStartCodeAndNal(csd1, pps, pps_size);

  auto make_format = [&](bool with_color, bool low_latency) {
    AMediaFormat* format = AMediaFormat_new();
    AMediaFormat_setString(format, AMEDIAFORMAT_KEY_MIME, "video/avc");
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_WIDTH, width);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_HEIGHT, height);
    AMediaFormat_setBuffer(format, "csd-0", csd0.data(), csd0.size());
    AMediaFormat_setBuffer(format, "csd-1", csd1.data(), csd1.size());
    if (with_color) {
      // Prefer NV12 byte-buffer output so we can convert without Flexible/Image APIs.
      AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_COLOR_FORMAT, kColorFormatNv12);
    }
    if (low_latency) {
      // AMEDIAFORMAT_KEY_LOW_LATENCY (API 30); the string key so the API-24 build compiles.
      AMediaFormat_setInt32(format, "low-latency", 1);
    }
    return format;
  };
  // Most capable first: low-latency output (API 30+, B125), then without it (some decoders reject
  // the key), then without the explicit color format (some devices reject that). MediaCodec may
  // have reset the component after a configure failure, so each retry starts from a new instance.
  struct ConfigureAttempt {
    bool with_color;
    bool low_latency;
  };
  ConfigureAttempt attempts[3];
  int attempt_count = 0;
  if (kTryLowLatencyDecoder && android_get_device_api_level() >= kLowLatencyApiLevel) {
    attempts[attempt_count++] = {true, true};
  }
  attempts[attempt_count++] = {true, false};
  attempts[attempt_count++] = {false, false};
  bool configured = false;
  for (int i = 0; i < attempt_count && !configured; ++i) {
    if (i > 0) {
      ReleaseHeldOutput();
      AMediaCodec_delete(decoder_);
      decoder_ = AMediaCodec_createDecoderByType("video/avc");
      if (!decoder_) {
        return Error("AMediaCodec_createDecoderByType failed on retry");
      }
    }
    AMediaFormat* format = make_format(attempts[i].with_color, attempts[i].low_latency);
    configured = AMediaCodec_configure(decoder_, format, nullptr, nullptr, 0) == AMEDIA_OK;
    AMediaFormat_delete(format);
    low_latency_ = configured && attempts[i].low_latency;
    if (!configured && attempts[i].low_latency) {
      DecoderLog().warning << "decoder rejected low-latency; configuring without it";
    }
  }
  if (!configured) {
    return fail_decoder("AMediaCodec_configure decoder failed");
  }
  if (AMediaCodec_start(decoder_) != AMEDIA_OK) {
    return fail_decoder("AMediaCodec_start decoder failed");
  }

  cached_sps_.assign(sps, sps + sps_size);
  cached_pps_.assign(pps, pps + pps_size);
  dec_frame_index_ = 0;
  dec_color_format_ = kColorFormatNv12;
  stall_.Restart(SteadyMs());
  configured_at_ms_ = SteadyMs();
  queued_since_configure_ = 0;
  first_output_logged_ = false;
  return {};
}

Roe<std::optional<VideoFrameRgba>> AndroidVideoCodec::Decode(const uint8_t* annex_b, size_t size) {
  const int64_t started_ms = SteadyMs();
  configure_ms_in_decode_ = 0;
  auto result = DecodeAccessUnit(annex_b, size);
  const int64_t now = SteadyMs();
  if (!result) {
    // The engine asks for a keyframe on every decode error: frames until it lands are not counted.
    stall_.NoteKeyframeRequested(now);
  }
  // Configure time is logged with the first frame (configure_ms), not as a slow decode.
  const int64_t decode_ms = now - started_ms - configure_ms_in_decode_;
  if (decode_ms > kSlowDecodeMs && slow_decode_warns_ < kMaxDecoderWarns) {
    ++slow_decode_warns_;
    DecoderLog().warning << "slow decode ms=" << decode_ms << " size=" << size
                         << " result=" << (!result ? "error" : (result->has_value() ? "frame" : "none"));
  }
  return result;
}

Roe<std::optional<VideoFrameRgba>> AndroidVideoCodec::DecodeAccessUnit(const uint8_t* annex_b, size_t size) {
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

  std::vector<uint8_t> slice_annex_b;
  bool has_slice = false;
  bool has_idr = false;
  bool sps_updated = false;
  bool pps_updated = false;

  for (const auto& nal : nals) {
    if (nal.type == 7) {
      if (cached_sps_.size() != nal.size || !std::equal(cached_sps_.begin(), cached_sps_.end(), nal.data)) {
        cached_sps_.assign(nal.data, nal.data + nal.size);
        sps_updated = true;
      }
      continue;
    }
    if (nal.type == 8) {
      if (cached_pps_.size() != nal.size || !std::equal(cached_pps_.begin(), cached_pps_.end(), nal.data)) {
        cached_pps_.assign(nal.data, nal.data + nal.size);
        pps_updated = true;
      }
      continue;
    }
    // VCL slices only (ignore AUD/SEI/filler). Matches Linux decoder.
    if (nal.type == 1 || nal.type == 5) {
      has_slice = true;
      has_idr = has_idr || nal.type == 5;
      AppendStartCodeAndNal(slice_annex_b, nal.data, nal.size);
    }
  }

  // Cache SPS/PPS across access units (Linux path). MF often emits parameter
  // sets in a separate AU from the IDR; requiring both in one AU left Android
  // waiting forever while Linux still decoded.
  if (!cached_sps_.empty() && !cached_pps_.empty() && (sps_updated || pps_updated || !decoder_)) {
    if ((sps_updated || pps_updated) && decoder_) {
      // cached_* already hold the new sets; drop the old codec so Ensure does
      // not treat the update as a no-op via the unchanged check.
      ReleaseHeldOutput();
      AMediaCodec_stop(decoder_);
      AMediaCodec_delete(decoder_);
      decoder_ = nullptr;
    }
    const int64_t configure_from = SteadyMs();
    auto cfg = EnsureDecoderConfigured(cached_sps_.data(), cached_sps_.size(), cached_pps_.data(), cached_pps_.size());
    configure_ms_in_decode_ = SteadyMs() - configure_from;
    if (!cfg) {
      return cfg.error();
    }
    configure_ms_ = configure_ms_in_decode_;
  }
  if (!decoder_) {
    return Error("H264 decoder waiting for SPS/PPS");
  }
  if (!has_slice || slice_annex_b.empty()) {
    return Error("access unit contained no slice data");
  }
  if (idr_wait_.Waiting()) {
    if (!has_idr) {
      // A frame was lost (or the codec rebuilt): frames that reference it would smear. Drop it.
      if (input_starved_frames_ > 0 && ++input_starved_frames_ >= kInputStarvedStallFrames) {
        input_starved_frames_ = 0;
        if (stall_warns_ < kMaxDecoderWarns) {
          ++stall_warns_;
          DecoderLog().warning << "decoder stalled: no input buffer for " << kInputStarvedStallFrames
                               << " frames drops=" << input_drops_;
        }
        return Error(std::string(kDecoderStalledError));
      }
      if (idr_wait_.AskAgain(SteadyMs())) {
        // The keyframe request or the IDR may have been lost: an error makes the engine ask again.
        return Error("waiting for keyframe");
      }
      return std::optional<VideoFrameRgba>();
    }
    idr_wait_.Stop();
  }

  // B125: this runs under the engine's media lock, so never wait long. No input buffer → take the
  // ready outputs (that frees input buffers) and try once more; still none → drop this frame and
  // everything up to the next IDR, and fail so the engine asks for one now.
  ssize_t buf_index = AMediaCodec_dequeueInputBuffer(decoder_, kDecodeInputTimeoutUs);
  if (buf_index < 0) {
    DrainOutputs();
    buf_index = AMediaCodec_dequeueInputBuffer(decoder_, kDecodeInputRetryTimeoutUs);
  }
  if (buf_index < 0) {
    ReleaseHeldOutput();
    idr_wait_.Start(SteadyMs());
    ++input_drops_;
    if (++input_starved_frames_ >= kInputStarvedStallFrames) {
      // Input has not flowed for many frames: report a stall so the engine's rebuild budget applies.
      input_starved_frames_ = 0;
      if (stall_warns_ < kMaxDecoderWarns) {
        ++stall_warns_;
        DecoderLog().warning << "decoder stalled: no input buffer for " << kInputStarvedStallFrames
                             << " frames drops=" << input_drops_;
      }
      return Error(std::string(kDecoderStalledError));
    }
    const int64_t now = SteadyMs();
    if (now - last_input_drop_warn_ms_ >= kInputDropWarnEveryMs && input_drop_warns_ < kMaxDecoderWarns) {
      ++input_drop_warns_;
      last_input_drop_warn_ms_ = now;
      DecoderLog().warning << "decoder input buffer unavailable: frame dropped, dropping until the next IDR"
                           << " drops=" << input_drops_;
    }
    return Error("decoder input buffer unavailable");
  }
  size_t buf_size = 0;
  uint8_t* buf = AMediaCodec_getInputBuffer(decoder_, static_cast<size_t>(buf_index), &buf_size);
  if (!buf || buf_size < slice_annex_b.size()) {
    ReleaseHeldOutput();
    return Error("decoder input buffer too small");
  }
  input_starved_frames_ = 0;
  std::memcpy(buf, slice_annex_b.data(), slice_annex_b.size());
  const int64_t pts_us = dec_frame_index_ * 66666LL; // ~15fps pacing hint
  ++dec_frame_index_;
  if (AMediaCodec_queueInputBuffer(decoder_, static_cast<size_t>(buf_index), 0, slice_annex_b.size(), pts_us, 0) !=
      AMEDIA_OK) {
    ReleaseHeldOutput();
    return Error("decoder queueInputBuffer failed");
  }
  stall_.NoteFrameQueued(SteadyMs());
  ++queued_since_configure_;

  DrainOutputs();
  if (held_output_index_ < 0) {
    // Nothing ready yet: a decoder filling its pipeline is not an error — unless it stays silent.
    const int64_t now = SteadyMs();
    if (stall_.Stalled(now)) {
      if (stall_warns_ < kMaxDecoderWarns) {
        ++stall_warns_;
        DecoderLog().warning << "decoder stalled: queued=" << stall_.FramesSinceOutput()
                             << " since_output_ms=" << stall_.MsSinceOutput(now);
      }
      stall_.NoteStallReported(now);
      return Error(std::string(kDecoderStalledError));
    }
    return std::optional<VideoFrameRgba>();
  }
  stall_.NoteOutput(SteadyMs());
  auto frame = ConvertHeldOutput();
  if (!frame) {
    return frame.error();
  }
  if (!first_output_logged_) {
    first_output_logged_ = true;
    DecoderLog().info << "first decoded frame codec=" << CodecName(decoder_)
                      << " queued_to_first=" << queued_since_configure_
                      << " ms_since_configure=" << (SteadyMs() - configured_at_ms_) << " configure_ms=" << configure_ms_
                      << " low_latency=" << (low_latency_ ? 1 : 0) << " input_drops=" << input_drops_
                      << " color_format=" << dec_color_format_ << " stride=" << dec_stride_ << " size=" << frame->width
                      << "x" << frame->height;
  }
  return std::optional<VideoFrameRgba>(std::move(*frame));
}

void AndroidVideoCodec::DrainOutputs() {
  for (int taken = 0; taken < kMaxDrainedOutputs; ++taken) {
    AMediaCodecBufferInfo info{};
    const ssize_t index = AMediaCodec_dequeueOutputBuffer(decoder_, &info, 0);
    if (index == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
      // A held buffer has the old layout and the new format is read at conversion: drop it.
      ReleaseHeldOutput();
      continue;
    }
    if (index == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) {
      continue; // not sent on NDK 21+
    }
    if (index < 0) {
      return; // TRY_AGAIN_LATER (nothing ready) or an error
    }
    // An older frame still held is stale next to this one: release it unconverted.
    ReleaseHeldOutput();
    held_output_index_ = index;
    held_output_info_ = info;
  }
}

void AndroidVideoCodec::ReleaseHeldOutput() {
  if (held_output_index_ >= 0) {
    AMediaCodec_releaseOutputBuffer(decoder_, static_cast<size_t>(held_output_index_), false);
    held_output_index_ = -1;
  }
}

Roe<VideoFrameRgba> AndroidVideoCodec::ConvertHeldOutput() {
  const size_t out_index = static_cast<size_t>(held_output_index_);
  const AMediaCodecBufferInfo info = held_output_info_;

  int width = 0;
  int height = 0;
  int stride = 0;
  int slice_height = 0;
  int crop_left = 0;
  int crop_top = 0;
  int crop_right = 0;
  int crop_bottom = 0;
  int color_format = dec_color_format_;
  AMediaFormat* out_format = AMediaCodec_getOutputFormat(decoder_);
  if (out_format) {
    AMediaFormat_getInt32(out_format, AMEDIAFORMAT_KEY_WIDTH, &width);
    AMediaFormat_getInt32(out_format, AMEDIAFORMAT_KEY_HEIGHT, &height);
    AMediaFormat_getInt32(out_format, AMEDIAFORMAT_KEY_COLOR_FORMAT, &color_format);
    AMediaFormat_getInt32(out_format, "stride", &stride);
    AMediaFormat_getInt32(out_format, "slice-height", &slice_height);
    AMediaFormat_getInt32(out_format, "crop-left", &crop_left);
    AMediaFormat_getInt32(out_format, "crop-top", &crop_top);
    AMediaFormat_getInt32(out_format, "crop-right", &crop_right);
    AMediaFormat_getInt32(out_format, "crop-bottom", &crop_bottom);
    AMediaFormat_delete(out_format);
    dec_color_format_ = color_format;
  }
  if (crop_right > crop_left && crop_bottom > crop_top) {
    width = crop_right - crop_left + 1;
    height = crop_bottom - crop_top + 1;
  }
  if (stride < width) {
    stride = width;
  }
  if (slice_height < height) {
    slice_height = height;
  }
  dec_stride_ = stride;

  VideoFrameRgba result;
  size_t out_size = 0;
  uint8_t* out_buf = AMediaCodec_getOutputBuffer(decoder_, out_index, &out_size);
  bool converted = false;
  if (out_buf && info.size > 0 && width > 0 && height > 0) {
    const uint8_t* base = out_buf + info.offset;
    const uint8_t* y =
        base + static_cast<size_t>(crop_top) * static_cast<size_t>(stride) + static_cast<size_t>(crop_left);
    if (color_format == kColorFormatYv12) {
      const uint8_t* u = base + static_cast<size_t>(slice_height) * static_cast<size_t>(stride) +
                         static_cast<size_t>(crop_top / 2) * static_cast<size_t>(stride / 2) +
                         static_cast<size_t>(crop_left / 2);
      const uint8_t* v = u + static_cast<size_t>(slice_height / 2) * static_cast<size_t>(stride / 2);
      converted = I420PlanesToRgba(y, u, v, width, height, stride, stride / 2, stride / 2, result);
    } else {
      // NV12 / semi-planar (default and most HW decoders).
      const uint8_t* uv = base + static_cast<size_t>(slice_height) * static_cast<size_t>(stride) +
                          static_cast<size_t>(crop_top / 2) * static_cast<size_t>(stride) +
                          static_cast<size_t>(crop_left & ~1);
      converted = Nv12ToRgba(y, uv, width, height, stride, stride, result);
    }
  }
  ReleaseHeldOutput();
  if (!converted) {
    return Error("failed to convert decoded frame to RGBA");
  }
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
  pending_param_sets_.clear();
}

void AndroidVideoCodec::ResetDecoder() {
  if (decoder_) {
    ReleaseHeldOutput();
    AMediaCodec_stop(decoder_);
    AMediaCodec_delete(decoder_);
    decoder_ = nullptr;
  }
  cached_sps_.clear();
  cached_pps_.clear();
  decoder_configured_ = false;
  dec_frame_index_ = 0;
  dec_color_format_ = kColorFormatNv12;
  // The next picture must be an IDR (B125: after a rebuild, or a call's first frames).
  idr_wait_.Start(SteadyMs());
  input_starved_frames_ = 0;
  stall_.Restart(SteadyMs());
}

} // namespace

// One class for both roles: a phone without an H.264 encoder still decodes (EncoderSupported says
// false, so no camera button; ConfigureEncoder fails there).
std::unique_ptr<IVideoCodec> CreateOsVideoCodec() {
  return std::make_unique<AndroidVideoCodec>();
}

} // namespace pbr

#endif // __ANDROID__
