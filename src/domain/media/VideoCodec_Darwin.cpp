#include "domain/media/IVideoCodec.h"
#include "domain/media/VideoCodecOs.h"
#include "domain/media/VideoCodecUnavailable.h"

#if defined(__APPLE__)

#include <TargetConditionals.h>
#include <CoreFoundation/CoreFoundation.h>
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <VideoToolbox/VideoToolbox.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

// V017: macOS H264 encode/decode via VideoToolbox. Encode sessions are
// created synchronously in ConfigureEncoder (dimensions are known up
// front). Decode sessions cannot be created until SPS/PPS parameter sets
// are observed in the bitstream, so they are created lazily on the first
// keyframe (see EnsureDecompressionSession) -- this is the standard
// VideoToolbox decode pattern, not a shortcut specific to this file.

namespace pbr {
namespace {

constexpr int64_t kMinBitrateBps = 200000;
constexpr int64_t kMaxBitrateBps = 4000000;

int32_t EstimateBitrateBps(int width, int height, int fps) {
  const double raw = static_cast<double>(width) * static_cast<double>(height) *
                      static_cast<double>(std::max(fps, 1)) * 0.07;
  const double clamped =
      std::clamp(raw, static_cast<double>(kMinBitrateBps), static_cast<double>(kMaxBitrateBps));
  return static_cast<int32_t>(clamped);
}

// ---- Annex-B parsing -------------------------------------------------

struct AnnexBNal {
  const uint8_t* data;
  size_t size;
  int type;
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

// ---- Pixel format conversion ------------------------------------------

// Packs planar I420 into an NV12 CVPixelBuffer (Y plane + interleaved UV);
// NV12 is VideoToolbox's native hardware encode input format.
void FillNv12PixelBuffer(CVPixelBufferRef pixel_buffer, const VideoFrameI420& frame) {
  CVPixelBufferLockBaseAddress(pixel_buffer, 0);

  auto* y_dst = static_cast<uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(pixel_buffer, 0));
  const size_t y_stride = CVPixelBufferGetBytesPerRowOfPlane(pixel_buffer, 0);
  auto* uv_dst = static_cast<uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(pixel_buffer, 1));
  const size_t uv_stride = CVPixelBufferGetBytesPerRowOfPlane(pixel_buffer, 1);

  const int width = frame.width;
  const int height = frame.height;
  for (int row = 0; row < height; ++row) {
    std::memcpy(y_dst + static_cast<size_t>(row) * y_stride,
                frame.y.data() + static_cast<size_t>(row) * width, width);
  }

  const int chroma_width = (width + 1) / 2;
  const int chroma_height = (height + 1) / 2;
  for (int row = 0; row < chroma_height; ++row) {
    const uint8_t* u_row = frame.u.data() + static_cast<size_t>(row) * chroma_width;
    const uint8_t* v_row = frame.v.data() + static_cast<size_t>(row) * chroma_width;
    uint8_t* uv_row = uv_dst + static_cast<size_t>(row) * uv_stride;
    for (int col = 0; col < chroma_width; ++col) {
      uv_row[col * 2 + 0] = u_row[col];
      uv_row[col * 2 + 1] = v_row[col];
    }
  }

  CVPixelBufferUnlockBaseAddress(pixel_buffer, 0);
}

// Decoder output is requested as 32BGRA (broadly supported); convert to the
// non-premultiplied RGBA byte order required by VideoFrameRgba.
VideoFrameRgba ConvertBgraPixelBufferToRgba(CVPixelBufferRef pixel_buffer) {
  CVPixelBufferLockBaseAddress(pixel_buffer, kCVPixelBufferLock_ReadOnly);

  const int width = static_cast<int>(CVPixelBufferGetWidth(pixel_buffer));
  const int height = static_cast<int>(CVPixelBufferGetHeight(pixel_buffer));
  const size_t stride = CVPixelBufferGetBytesPerRow(pixel_buffer);
  const auto* base = static_cast<const uint8_t*>(CVPixelBufferGetBaseAddress(pixel_buffer));

  VideoFrameRgba out;
  out.width = width;
  out.height = height;
  out.rgba.resize(static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
  for (int y = 0; y < height; ++y) {
    const uint8_t* src = base + static_cast<size_t>(y) * stride;
    uint8_t* dst = out.rgba.data() + static_cast<size_t>(y) * width * 4;
    for (int x = 0; x < width; ++x) {
      dst[x * 4 + 0] = src[x * 4 + 2]; // R
      dst[x * 4 + 1] = src[x * 4 + 1]; // G
      dst[x * 4 + 2] = src[x * 4 + 0]; // B
      dst[x * 4 + 3] = 255;
    }
  }

  CVPixelBufferUnlockBaseAddress(pixel_buffer, kCVPixelBufferLock_ReadOnly);
  return out;
}

// ---- Encode output handling ---------------------------------------------

struct EncodeOutput {
  std::vector<uint8_t> annex_b;
  bool keyframe = false;
  bool got_output = false;
  bool error = false;
};

// Converts a compressed CMSampleBuffer (AVCC: 4-byte length prefixed NALs,
// SPS/PPS carried out-of-band in the format description) into Annex-B
// (start-code prefixed, SPS/PPS inlined ahead of each keyframe).
void AppendEncodedSample(CMSampleBufferRef sample_buffer, EncodeOutput& out) {
  bool keyframe = true;
  CFArrayRef attachments = CMSampleBufferGetSampleAttachmentsArray(sample_buffer, false);
  if (attachments && CFArrayGetCount(attachments) > 0) {
    auto* dict = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(attachments, 0));
    auto* not_sync =
        static_cast<CFBooleanRef>(CFDictionaryGetValue(dict, kCMSampleAttachmentKey_NotSync));
    keyframe = !(not_sync && CFBooleanGetValue(not_sync));
  }

  if (keyframe) {
    CMFormatDescriptionRef format = CMSampleBufferGetFormatDescription(sample_buffer);
    if (format) {
      size_t param_count = 0;
      int header_length = 4;
      if (CMVideoFormatDescriptionGetH264ParameterSetAtIndex(format, 0, nullptr, nullptr,
                                                              &param_count, &header_length) ==
          noErr) {
        for (size_t i = 0; i < param_count; ++i) {
          const uint8_t* param_data = nullptr;
          size_t param_size = 0;
          if (CMVideoFormatDescriptionGetH264ParameterSetAtIndex(
                  format, i, &param_data, &param_size, nullptr, nullptr) == noErr) {
            AppendStartCodeAndNal(out.annex_b, param_data, param_size);
          }
        }
      }
    }
  }

  CMBlockBufferRef block = CMSampleBufferGetDataBuffer(sample_buffer);
  if (!block) {
    out.error = true;
    return;
  }
  const size_t total_length = CMBlockBufferGetDataLength(block);
  std::vector<uint8_t> payload(total_length);
  if (total_length > 0 &&
      CMBlockBufferCopyDataBytes(block, 0, total_length, payload.data()) != kCMBlockBufferNoErr) {
    out.error = true;
    return;
  }

  constexpr size_t kLengthPrefixSize = 4; // VideoToolbox always emits 4-byte AVCC lengths.
  size_t offset = 0;
  while (offset + kLengthPrefixSize <= total_length) {
    uint32_t nal_size = 0;
    for (size_t i = 0; i < kLengthPrefixSize; ++i) {
      nal_size = (nal_size << 8) | payload[offset + i];
    }
    offset += kLengthPrefixSize;
    if (offset + nal_size > total_length) {
      break;
    }
    AppendStartCodeAndNal(out.annex_b, payload.data() + offset, nal_size);
    offset += nal_size;
  }

  out.keyframe = keyframe;
  out.got_output = true;
}

class VideoToolboxVideoCodec final : public IVideoCodec {
public:
  VideoToolboxVideoCodec() = default;
  ~VideoToolboxVideoCodec() override;

  std::string BackendName() const override { return "videotoolbox"; }
  bool HasEncoder() const override { return compression_session_ != nullptr; }
  // A concrete VTDecompressionSession cannot be created until SPS/PPS is
  // observed in the bitstream (see class comment); decoder_configured_
  // reflects that VideoToolbox decode capability is ready to be used as
  // soon as a keyframe arrives.
  bool HasDecoder() const override { return decoder_configured_; }

  Roe<void> ConfigureEncoder(int width, int height, int fps) override;
  Roe<void> ConfigureDecoder() override;

  Roe<EncodedAccessUnit> Encode(const VideoFrameI420& frame, bool force_keyframe) override;
  Roe<VideoFrameRgba> Decode(const uint8_t* annex_b, size_t size) override;

  void ResetEncoder() override;
  void ResetDecoder() override;

private:
  bool EnsureDecompressionSession(const uint8_t* sps, size_t sps_size, const uint8_t* pps,
                                   size_t pps_size, std::string& error_out);

  static void CompressionOutputCallback(void* output_ref_con, void* source_frame_ref_con,
                                         OSStatus status, VTEncodeInfoFlags info_flags,
                                         CMSampleBufferRef sample_buffer);
  static void DecompressionOutputCallback(void* decompression_output_ref_con,
                                           void* source_frame_ref_con, OSStatus status,
                                           VTDecodeInfoFlags info_flags,
                                           CVImageBufferRef image_buffer,
                                           CMTime presentation_time_stamp,
                                           CMTime presentation_duration);

  VTCompressionSessionRef compression_session_ = nullptr;
  int enc_width_ = 0;
  int enc_height_ = 0;
  int enc_fps_ = 0;
  int64_t enc_frame_index_ = 0;
  EncodeOutput* pending_encode_output_ = nullptr;

  VTDecompressionSessionRef decompression_session_ = nullptr;
  CMFormatDescriptionRef decoder_format_ = nullptr;
  std::vector<uint8_t> cached_sps_;
  std::vector<uint8_t> cached_pps_;
  int decode_width_ = 0;
  int decode_height_ = 0;
  bool decoder_configured_ = false;
  CVPixelBufferRef pending_decode_pixel_buffer_ = nullptr;
  bool pending_decode_error_ = false;
};

VideoToolboxVideoCodec::~VideoToolboxVideoCodec() {
  ResetEncoder();
  ResetDecoder();
}

Roe<void> VideoToolboxVideoCodec::ConfigureEncoder(int width, int height, int fps) {
  ResetEncoder();
  if (width <= 0 || height <= 0 || fps <= 0) {
    return Error("invalid H264 encoder configuration");
  }

  CFMutableDictionaryRef encoder_spec = CFDictionaryCreateMutable(
      kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
  // On iOS this key is only public from 17.4; older OS still HW-encodes by default.
  // Without a guard, -Wunguarded-availability-new fails when targeting iOS < 17.4.
#if TARGET_OS_IPHONE
  if (__builtin_available(iOS 17.4, *)) {
    CFDictionarySetValue(encoder_spec,
                         kVTVideoEncoderSpecification_EnableHardwareAcceleratedVideoEncoder,
                         kCFBooleanTrue);
  }
#else
  CFDictionarySetValue(encoder_spec, kVTVideoEncoderSpecification_EnableHardwareAcceleratedVideoEncoder,
                       kCFBooleanTrue);
#endif
#if defined(kVTVideoEncoderSpecification_EnableLowLatencyRateControl)
  // Prefer 1-in-1-out HW encode so CompleteFrames does not flush multi-frame Annex-B blobs
  // that break Linux's VA-API slice decoder (Android MediaCodec tolerates them).
  CFDictionarySetValue(encoder_spec, kVTVideoEncoderSpecification_EnableLowLatencyRateControl,
                       kCFBooleanTrue);
#endif

  CFMutableDictionaryRef source_attrs = CFDictionaryCreateMutable(
      kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
  const int pixel_format = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
  CFNumberRef pixel_format_number = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &pixel_format);
  CFDictionarySetValue(source_attrs, kCVPixelBufferPixelFormatTypeKey, pixel_format_number);
  CFRelease(pixel_format_number);

  VTCompressionSessionRef session = nullptr;
  OSStatus status = VTCompressionSessionCreate(
      kCFAllocatorDefault, width, height, kCMVideoCodecType_H264, encoder_spec, source_attrs,
      nullptr, &VideoToolboxVideoCodec::CompressionOutputCallback, this, &session);
  CFRelease(encoder_spec);
  CFRelease(source_attrs);

  if (status != noErr || !session) {
    // Retry without requesting hardware in case only a software path exists.
    status = VTCompressionSessionCreate(kCFAllocatorDefault, width, height, kCMVideoCodecType_H264,
                                         nullptr, nullptr, nullptr,
                                         &VideoToolboxVideoCodec::CompressionOutputCallback, this,
                                         &session);
  }
  if (status != noErr || !session) {
    return Error("VTCompressionSessionCreate failed");
  }

  // Constrained Baseline is required for Linux VA-API CBP hosts and WebRTC interop.
  // Ignore failures only after trying Baseline as a fallback — never leave the
  // session on an implicit Main/High default (CABAC breaks the Linux decoder).
  status = VTSessionSetProperty(session, kVTCompressionPropertyKey_ProfileLevel,
                                kVTProfileLevel_H264_ConstrainedBaseline_AutoLevel);
  if (status != noErr) {
    status = VTSessionSetProperty(session, kVTCompressionPropertyKey_ProfileLevel,
                                  kVTProfileLevel_H264_Baseline_AutoLevel);
  }
  if (status != noErr) {
    VTCompressionSessionInvalidate(session);
    CFRelease(session);
    return Error("VTCompressionSession refused Baseline/Constrained Baseline profile");
  }
  VTSessionSetProperty(session, kVTCompressionPropertyKey_RealTime, kCFBooleanTrue);
  VTSessionSetProperty(session, kVTCompressionPropertyKey_AllowFrameReordering, kCFBooleanFalse);
  const int32_t max_delay = 0;
  CFNumberRef max_delay_number = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &max_delay);
  VTSessionSetProperty(session, kVTCompressionPropertyKey_MaxFrameDelayCount, max_delay_number);
  CFRelease(max_delay_number);

  const int32_t max_keyframe_interval = fps * 2;
  CFNumberRef max_keyframe_number =
      CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &max_keyframe_interval);
  VTSessionSetProperty(session, kVTCompressionPropertyKey_MaxKeyFrameInterval, max_keyframe_number);
  CFRelease(max_keyframe_number);

  const int32_t fps32 = fps;
  CFNumberRef fps_number = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &fps32);
  VTSessionSetProperty(session, kVTCompressionPropertyKey_ExpectedFrameRate, fps_number);
  CFRelease(fps_number);

  const int32_t bitrate = EstimateBitrateBps(width, height, fps);
  CFNumberRef bitrate_number = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &bitrate);
  VTSessionSetProperty(session, kVTCompressionPropertyKey_AverageBitRate, bitrate_number);
  CFRelease(bitrate_number);

  status = VTCompressionSessionPrepareToEncodeFrames(session);
  if (status != noErr) {
    VTCompressionSessionInvalidate(session);
    CFRelease(session);
    return Error("VTCompressionSessionPrepareToEncodeFrames failed");
  }

  compression_session_ = session;
  enc_width_ = width;
  enc_height_ = height;
  enc_fps_ = fps;
  enc_frame_index_ = 0;
  return {};
}

Roe<EncodedAccessUnit> VideoToolboxVideoCodec::Encode(const VideoFrameI420& frame,
                                                       bool force_keyframe) {
  if (!compression_session_) {
    return Error("H264 encoder not configured");
  }
  if (frame.width != enc_width_ || frame.height != enc_height_) {
    return Error("encoder frame size does not match ConfigureEncoder size");
  }

  CVPixelBufferPoolRef pool = VTCompressionSessionGetPixelBufferPool(compression_session_);
  CVPixelBufferRef pixel_buffer = nullptr;
  CVReturn cv_status =
      pool ? CVPixelBufferPoolCreatePixelBuffer(kCFAllocatorDefault, pool, &pixel_buffer)
           : CVPixelBufferCreate(kCFAllocatorDefault, frame.width, frame.height,
                                  kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange, nullptr,
                                  &pixel_buffer);
  if (cv_status != kCVReturnSuccess || !pixel_buffer) {
    return Error("failed to allocate encoder pixel buffer");
  }
  FillNv12PixelBuffer(pixel_buffer, frame);

  CFMutableDictionaryRef frame_properties = nullptr;
  if (force_keyframe) {
    frame_properties = CFDictionaryCreateMutable(kCFAllocatorDefault, 1,
                                                  &kCFTypeDictionaryKeyCallBacks,
                                                  &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(frame_properties, kVTEncodeFrameOptionKey_ForceKeyFrame, kCFBooleanTrue);
  }

  const CMTime pts = CMTimeMake(enc_frame_index_, enc_fps_);
  const CMTime duration = CMTimeMake(1, enc_fps_);
  ++enc_frame_index_;

  EncodeOutput output;
  pending_encode_output_ = &output;
  VTEncodeInfoFlags info_flags = 0;
  const OSStatus status = VTCompressionSessionEncodeFrame(
      compression_session_, pixel_buffer, pts, duration, frame_properties, nullptr, &info_flags);
  if (frame_properties) {
    CFRelease(frame_properties);
  }
  CVPixelBufferRelease(pixel_buffer);

  if (status == noErr) {
    // Force the (inherently asynchronous) compression session to flush its
    // callback for this frame before Encode() returns, since IVideoCodec is
    // a synchronous interface.
    VTCompressionSessionCompleteFrames(compression_session_, kCMTimeInvalid);
  }
  pending_encode_output_ = nullptr;

  if (status != noErr) {
    return Error("VTCompressionSessionEncodeFrame failed");
  }
  if (output.error) {
    return Error("H264 encode callback reported failure");
  }
  if (!output.got_output || output.annex_b.empty()) {
    // Encoder may drop/delay under load; do not emit an empty RTP access unit.
    return Error("H264 encode produced no output");
  }

  EncodedAccessUnit unit;
  unit.annex_b = std::move(output.annex_b);
  unit.keyframe = output.keyframe;
  return unit;
}

Roe<void> VideoToolboxVideoCodec::ConfigureDecoder() {
  ResetDecoder();
  decoder_configured_ = true;
  return {};
}

bool VideoToolboxVideoCodec::EnsureDecompressionSession(const uint8_t* sps, size_t sps_size,
                                                         const uint8_t* pps, size_t pps_size,
                                                         std::string& error_out) {
  const bool unchanged = decompression_session_ && cached_sps_.size() == sps_size &&
                          cached_pps_.size() == pps_size &&
                          std::equal(cached_sps_.begin(), cached_sps_.end(), sps) &&
                          std::equal(cached_pps_.begin(), cached_pps_.end(), pps);
  if (unchanged) {
    return true;
  }

  const uint8_t* param_ptrs[2] = {sps, pps};
  const size_t param_sizes[2] = {sps_size, pps_size};
  CMFormatDescriptionRef format = nullptr;
  OSStatus status = CMVideoFormatDescriptionCreateFromH264ParameterSets(
      kCFAllocatorDefault, 2, param_ptrs, param_sizes, 4, &format);
  if (status != noErr || !format) {
    error_out = "failed to build H264 format description from SPS/PPS";
    return false;
  }

  CFMutableDictionaryRef dest_attrs = CFDictionaryCreateMutable(
      kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
  const int pixel_format = kCVPixelFormatType_32BGRA;
  CFNumberRef pixel_format_number = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &pixel_format);
  CFDictionarySetValue(dest_attrs, kCVPixelBufferPixelFormatTypeKey, pixel_format_number);
  CFRelease(pixel_format_number);

  VTDecompressionOutputCallbackRecord callback{};
  callback.decompressionOutputCallback = &VideoToolboxVideoCodec::DecompressionOutputCallback;
  callback.decompressionOutputRefCon = this;

  VTDecompressionSessionRef session = nullptr;
  status = VTDecompressionSessionCreate(kCFAllocatorDefault, format, nullptr, dest_attrs, &callback,
                                         &session);
  CFRelease(dest_attrs);
  if (status != noErr || !session) {
    CFRelease(format);
    error_out = "VTDecompressionSessionCreate failed";
    return false;
  }

  if (decompression_session_) {
    VTDecompressionSessionInvalidate(decompression_session_);
    CFRelease(decompression_session_);
  }
  if (decoder_format_) {
    CFRelease(decoder_format_);
  }
  decompression_session_ = session;
  decoder_format_ = format;
  cached_sps_.assign(sps, sps + sps_size);
  cached_pps_.assign(pps, pps + pps_size);

  const CMVideoDimensions dims = CMVideoFormatDescriptionGetDimensions(format);
  decode_width_ = dims.width;
  decode_height_ = dims.height;
  return true;
}

Roe<VideoFrameRgba> VideoToolboxVideoCodec::Decode(const uint8_t* annex_b, size_t size) {
  if (!decoder_configured_) {
    return Error("H264 decoder not configured");
  }
  if (!annex_b || size == 0) {
    return Error("empty H264 decoder input");
  }

  const std::vector<AnnexBNal> nals = ParseAnnexB(annex_b, size);
  if (nals.empty()) {
    return Error("no NAL units found in access unit");
  }

  const uint8_t* sps = nullptr;
  size_t sps_size = 0;
  const uint8_t* pps = nullptr;
  size_t pps_size = 0;
  std::vector<uint8_t> avcc_payload;
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
    // VCL slices only (ignore AUD/SEI/filler). Matches Linux/Android decoders.
    if (nal.type != 1 && nal.type != 5) {
      continue;
    }
    has_slice = true;
    const uint32_t nal_size = static_cast<uint32_t>(nal.size);
    const uint8_t length_prefix[4] = {
        static_cast<uint8_t>((nal_size >> 24) & 0xFF), static_cast<uint8_t>((nal_size >> 16) & 0xFF),
        static_cast<uint8_t>((nal_size >> 8) & 0xFF), static_cast<uint8_t>(nal_size & 0xFF)};
    avcc_payload.insert(avcc_payload.end(), length_prefix, length_prefix + 4);
    avcc_payload.insert(avcc_payload.end(), nal.data, nal.data + nal.size);
  }

  if (sps && pps) {
    std::string reason;
    if (!EnsureDecompressionSession(sps, sps_size, pps, pps_size, reason)) {
      return Error(reason);
    }
  }
  if (!decompression_session_) {
    return Error("H264 decoder waiting for a keyframe with SPS/PPS");
  }
  if (!has_slice) {
    return Error("access unit contained no slice data");
  }

  CMBlockBufferRef block = nullptr;
  OSStatus status = CMBlockBufferCreateWithMemoryBlock(kCFAllocatorDefault, nullptr,
                                                        avcc_payload.size(), kCFAllocatorDefault,
                                                        nullptr, 0, avcc_payload.size(), 0, &block);
  if (status != kCMBlockBufferNoErr || !block) {
    return Error("failed to allocate decoder block buffer");
  }
  status = CMBlockBufferReplaceDataBytes(avcc_payload.data(), block, 0, avcc_payload.size());
  if (status != kCMBlockBufferNoErr) {
    CFRelease(block);
    return Error("failed to fill decoder block buffer");
  }

  const size_t sample_size = avcc_payload.size();
  CMSampleBufferRef sample = nullptr;
  status = CMSampleBufferCreateReady(kCFAllocatorDefault, block, decoder_format_, 1, 0, nullptr, 1,
                                      &sample_size, &sample);
  CFRelease(block);
  if (status != noErr || !sample) {
    return Error("failed to build decoder sample buffer");
  }

  pending_decode_pixel_buffer_ = nullptr;
  pending_decode_error_ = false;
  VTDecodeInfoFlags info_flags = 0;
  status = VTDecompressionSessionDecodeFrame(decompression_session_, sample, 0, nullptr, &info_flags);
  CFRelease(sample);

  if (status != noErr) {
    pending_decode_pixel_buffer_ = nullptr;
    return Error("VTDecompressionSessionDecodeFrame failed");
  }
  if (pending_decode_error_ || !pending_decode_pixel_buffer_) {
    return Error("H264 decode produced no frame for this access unit");
  }

  VideoFrameRgba result = ConvertBgraPixelBufferToRgba(pending_decode_pixel_buffer_);
  CVPixelBufferRelease(pending_decode_pixel_buffer_);
  pending_decode_pixel_buffer_ = nullptr;
  return result;
}

void VideoToolboxVideoCodec::ResetEncoder() {
  if (compression_session_) {
    VTCompressionSessionInvalidate(compression_session_);
    CFRelease(compression_session_);
    compression_session_ = nullptr;
  }
  enc_width_ = 0;
  enc_height_ = 0;
  enc_fps_ = 0;
  enc_frame_index_ = 0;
}

void VideoToolboxVideoCodec::ResetDecoder() {
  if (decompression_session_) {
    VTDecompressionSessionInvalidate(decompression_session_);
    CFRelease(decompression_session_);
    decompression_session_ = nullptr;
  }
  if (decoder_format_) {
    CFRelease(decoder_format_);
    decoder_format_ = nullptr;
  }
  cached_sps_.clear();
  cached_pps_.clear();
  decode_width_ = 0;
  decode_height_ = 0;
  decoder_configured_ = false;
}

void VideoToolboxVideoCodec::CompressionOutputCallback(void* output_ref_con, void*, OSStatus status,
                                                        VTEncodeInfoFlags,
                                                        CMSampleBufferRef sample_buffer) {
  auto* self = static_cast<VideoToolboxVideoCodec*>(output_ref_con);
  if (!self || !self->pending_encode_output_) {
    return;
  }
  EncodeOutput& out = *self->pending_encode_output_;
  if (status != noErr || !sample_buffer || !CMSampleBufferDataIsReady(sample_buffer)) {
    out.error = true;
    return;
  }
  // CompleteFrames can flush more than one delayed frame into this Encode() call.
  // Keep only the latest AU so Linux does not see concatenated multi-frame Annex-B.
  out.annex_b.clear();
  out.got_output = false;
  out.keyframe = false;
  AppendEncodedSample(sample_buffer, out);
}

void VideoToolboxVideoCodec::DecompressionOutputCallback(void* decompression_output_ref_con, void*,
                                                          OSStatus status, VTDecodeInfoFlags,
                                                          CVImageBufferRef image_buffer, CMTime,
                                                          CMTime) {
  auto* self = static_cast<VideoToolboxVideoCodec*>(decompression_output_ref_con);
  if (!self) {
    return;
  }
  if (status != noErr || !image_buffer) {
    self->pending_decode_error_ = true;
    return;
  }
  self->pending_decode_pixel_buffer_ = CVPixelBufferRetain(image_buffer);
}

} // namespace

std::unique_ptr<IVideoCodec> CreateOsVideoCodec() {
  // Cheap capability probe mirroring the Win32 Create*-time failure
  // contract: if this device cannot stand up an H264 compression session at
  // all, fall back to the unavailable stub instead of returning a codec
  // whose ConfigureEncoder() would merely fail every time.
  VTCompressionSessionRef probe_session = nullptr;
  const OSStatus status = VTCompressionSessionCreate(kCFAllocatorDefault, 64, 64, kCMVideoCodecType_H264,
                                                      nullptr, nullptr, nullptr, nullptr, nullptr,
                                                      &probe_session);
  if (status != noErr || !probe_session) {
    return MakeUnavailableVideoCodec("VideoToolbox H264 encode unavailable on this device");
  }
  VTCompressionSessionInvalidate(probe_session);
  CFRelease(probe_session);

  return std::make_unique<VideoToolboxVideoCodec>();
}

} // namespace pbr

#endif // defined(__APPLE__)
