#include "base/media/IVideoCodec.h"
#include "base/media/VideoCodecOs.h"
#include "base/media/VideoCodecUnavailable.h"
#include "base/media/VideoYuv.h"

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <codecapi.h>
#include <icodecapi.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfobjects.h>
#include <mftransform.h>
#include <oleauto.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

// V017: Windows H264 encode/decode via Media Foundation. Hardware MFTs are
// preferred (see EnumerateTransforms) with software MFTs as fallback so a
// missing HW encoder never blocks call bring-up (V019).

namespace pbr {
namespace {

using Microsoft::WRL::ComPtr;

constexpr UINT32 kMinBitrateBps = 200000;
constexpr UINT32 kMaxBitrateBps = 4000000;
constexpr int kDefaultDecodeWidth = 640;
constexpr int kDefaultDecodeHeight = 360;
// Hardware MFTs are asynchronous; bound how long we will spin waiting on
// MFT events so a stalled transform cannot hang the call media thread.
constexpr auto kAsyncOverallTimeout = std::chrono::milliseconds(400);
constexpr auto kAsyncPostInputTimeout = std::chrono::milliseconds(120);

UINT32 EstimateBitrateBps(int width, int height, int fps) {
  const double raw = static_cast<double>(width) * static_cast<double>(height) *
                      static_cast<double>(std::max(fps, 1)) * 0.07;
  return static_cast<UINT32>(
      std::clamp(raw, static_cast<double>(kMinBitrateBps), static_cast<double>(kMaxBitrateBps)));
}

// Enumerates MFTs for a category/type pair. `prefer_hardware` puts HW MFTs
// first (encoders); decoders often prefer sync software for reliable CPU
// output formats without DXVA device binding.
std::vector<ComPtr<IMFTransform>> EnumerateTransforms(GUID category,
                                                       const MFT_REGISTER_TYPE_INFO* input_info,
                                                       const MFT_REGISTER_TYPE_INFO* output_info,
                                                       bool prefer_hardware = true) {
  std::vector<ComPtr<IMFTransform>> transforms;
  const UINT32 hw_flags = MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER;
  const UINT32 sw_flags =
      MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_ASYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER;
  const UINT32 flag_passes[2] = {prefer_hardware ? hw_flags : sw_flags,
                                 prefer_hardware ? sw_flags : hw_flags};
  for (UINT32 flags : flag_passes) {
    IMFActivate** activates = nullptr;
    UINT32 count = 0;
    const HRESULT hr =
        MFTEnumEx(category, flags, input_info, output_info, &activates, &count);
    if (SUCCEEDED(hr)) {
      for (UINT32 i = 0; i < count; ++i) {
        if (!activates[i]) {
          continue;
        }
        ComPtr<IMFTransform> transform;
        if (SUCCEEDED(activates[i]->ActivateObject(IID_PPV_ARGS(&transform)))) {
          transforms.push_back(std::move(transform));
        }
        activates[i]->Release();
      }
    }
    if (activates) {
      CoTaskMemFree(activates);
    }
  }
  return transforms;
}

// Hardware/async MFTs reject every call until MF_TRANSFORM_ASYNC_UNLOCK is
// set on their attribute store. Returns true if the transform is async.
bool UnlockAsyncIfNeeded(IMFTransform* transform) {
  ComPtr<IMFAttributes> attributes;
  if (FAILED(transform->GetAttributes(&attributes)) || !attributes) {
    return false;
  }
  UINT32 is_async = 0;
  attributes->GetUINT32(MF_TRANSFORM_ASYNC, &is_async);
  if (is_async) {
    attributes->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
  }
  return is_async != 0;
}

bool AnnexBContainsIdr(const uint8_t* data, size_t size) {
  size_t i = 0;
  while (i + 3 < size) {
    size_t sc = 0;
    if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
      sc = 3;
    } else if (i + 4 < size && data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 0 && data[i + 3] == 1) {
      sc = 4;
    } else {
      ++i;
      continue;
    }
    if (i + sc < size && ((data[i + sc] & 0x1F) == 5)) {
      return true;
    }
    i += sc;
  }
  return false;
}

HRESULT ProcessOneOutput(IMFTransform* transform, DWORD output_stream_id, DWORD buffer_size,
                          bool provides_samples, ComPtr<IMFSample>& out_sample) {
  MFT_OUTPUT_DATA_BUFFER data{};
  data.dwStreamID = output_stream_id;
  ComPtr<IMFSample> local_sample;
  if (!provides_samples) {
    ComPtr<IMFMediaBuffer> local_buffer;
    const DWORD alloc_size = buffer_size ? buffer_size : 1;
    if (FAILED(MFCreateSample(&local_sample)) ||
        FAILED(MFCreateMemoryBuffer(alloc_size, &local_buffer)) ||
        FAILED(local_sample->AddBuffer(local_buffer.Get()))) {
      return E_OUTOFMEMORY;
    }
    data.pSample = local_sample.Get();
  }
  DWORD status = 0;
  const HRESULT hr = transform->ProcessOutput(0, 1, &data, &status);
  if (data.pEvents) {
    data.pEvents->Release();
  }
  if (FAILED(hr)) {
    return hr;
  }
  if (provides_samples) {
    out_sample.Attach(data.pSample);
  } else {
    out_sample = local_sample;
  }
  return S_OK;
}

struct PumpOutcome {
  std::vector<ComPtr<IMFSample>> samples;
  bool stream_change = false;
  bool error = false;
  HRESULT hr = S_OK;
};

// Feeds `input_sample` (may be null to only drain pending output) to
// `transform` and collects whatever output samples become available for
// this call. Handles both classic synchronous MFTs and hardware/async MFTs
// driven via IMFMediaEventGenerator, bounding the wait on the latter.
PumpOutcome PumpTransform(IMFTransform* transform, IMFMediaEventGenerator* events, bool is_async,
                           DWORD output_stream_id, DWORD output_buffer_size, bool provides_samples,
                           IMFSample* input_sample) {
  PumpOutcome outcome;
  bool input_fed = (input_sample == nullptr);

  auto drain_once = [&]() -> bool {
    ComPtr<IMFSample> sample;
    const HRESULT hr =
        ProcessOneOutput(transform, output_stream_id, output_buffer_size, provides_samples, sample);
    outcome.hr = hr;
    if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
      return false;
    }
    // TYPE_NOT_SET: output type never negotiated yet (first frame).
    // STREAM_CHANGE: SPS/resolution change. Both require SetOutputType + retry.
    if (hr == MF_E_TRANSFORM_STREAM_CHANGE || hr == MF_E_TRANSFORM_TYPE_NOT_SET) {
      outcome.stream_change = true;
      return false;
    }
    if (FAILED(hr)) {
      outcome.error = true;
      return false;
    }
    outcome.samples.push_back(std::move(sample));
    return true;
  };

  if (!is_async) {
    if (!input_fed) {
      const HRESULT hr = transform->ProcessInput(0, input_sample, 0);
      outcome.hr = hr;
      if (SUCCEEDED(hr)) {
        input_fed = true;
      } else if (hr != MF_E_NOTACCEPTING) {
        outcome.error = true;
        return outcome;
      }
    }
    while (drain_once()) {
    }
    if (!input_fed && !outcome.error && !outcome.stream_change) {
      const HRESULT hr = transform->ProcessInput(0, input_sample, 0);
      outcome.hr = hr;
      if (FAILED(hr)) {
        outcome.error = true;
        return outcome;
      }
      while (drain_once()) {
      }
    }
    return outcome;
  }

  if (!events) {
    outcome.error = true;
    outcome.hr = E_POINTER;
    return outcome;
  }

  // Async (hardware) path: only call ProcessInput/ProcessOutput in response
  // to the matching MFT event, per the Media Foundation async MFT contract.
  const auto overall_deadline = std::chrono::steady_clock::now() + kAsyncOverallTimeout;
  std::chrono::steady_clock::time_point post_input_deadline{};
  bool have_post_input_deadline = false;

  while (std::chrono::steady_clock::now() < overall_deadline &&
         (!have_post_input_deadline || std::chrono::steady_clock::now() < post_input_deadline)) {
    ComPtr<IMFMediaEvent> event;
    const HRESULT hr = events->GetEvent(MF_EVENT_FLAG_NO_WAIT, &event);
    if (hr == MF_E_NO_EVENTS_AVAILABLE) {
      Sleep(1);
      continue;
    }
    if (FAILED(hr)) {
      outcome.error = true;
      outcome.hr = hr;
      break;
    }
    MediaEventType type = MEUnknown;
    event->GetType(&type);
    if (type == METransformNeedInput) {
      if (input_fed) {
        // MFT wants more data than this call provides; nothing left to do.
        break;
      }
      const HRESULT input_hr = transform->ProcessInput(0, input_sample, 0);
      outcome.hr = input_hr;
      if (FAILED(input_hr)) {
        outcome.error = true;
        break;
      }
      input_fed = true;
      post_input_deadline = std::chrono::steady_clock::now() + kAsyncPostInputTimeout;
      have_post_input_deadline = true;
    } else if (type == METransformHaveOutput) {
      const bool got_sample = drain_once();
      if (outcome.error || outcome.stream_change) {
        break;
      }
      if (got_sample && input_fed) {
        // Real-time baseline config is 1-in/1-out; stop once we have it.
        break;
      }
    } else if (type == METransformDrainComplete) {
      break;
    }
  }
  return outcome;
}

bool AppendSampleBytes(IMFSample* sample, std::vector<uint8_t>& out) {
  ComPtr<IMFMediaBuffer> buffer;
  if (FAILED(sample->ConvertToContiguousBuffer(&buffer))) {
    return false;
  }
  BYTE* data = nullptr;
  DWORD current_len = 0;
  if (FAILED(buffer->Lock(&data, nullptr, &current_len))) {
    return false;
  }
  out.insert(out.end(), data, data + current_len);
  buffer->Unlock();
  return true;
}

// Packs planar I420 into an NV12 IMFSample (Y plane + interleaved UV) since
// NV12 is the widely-supported hardware encoder input format on Windows.
HRESULT BuildNv12Sample(const VideoFrameI420& frame, LONGLONG timestamp, LONGLONG duration,
                         ComPtr<IMFSample>& out_sample) {
  const int width = frame.width;
  const int height = frame.height;
  const size_t luma_size = static_cast<size_t>(width) * static_cast<size_t>(height);
  const size_t frame_size = luma_size + luma_size / 2;

  ComPtr<IMFMediaBuffer> buffer;
  HRESULT hr = MFCreateMemoryBuffer(static_cast<DWORD>(frame_size), &buffer);
  if (FAILED(hr)) {
    return hr;
  }
  BYTE* dst = nullptr;
  hr = buffer->Lock(&dst, nullptr, nullptr);
  if (FAILED(hr)) {
    return hr;
  }
  std::memcpy(dst, frame.y.data(), luma_size);

  BYTE* uv = dst + luma_size;
  const int chroma_width = (width + 1) / 2;
  const int chroma_height = (height + 1) / 2;
  for (int row = 0; row < chroma_height; ++row) {
    const uint8_t* u_row = frame.u.data() + static_cast<size_t>(row) * chroma_width;
    const uint8_t* v_row = frame.v.data() + static_cast<size_t>(row) * chroma_width;
    BYTE* uv_row = uv + static_cast<size_t>(row) * chroma_width * 2;
    for (int col = 0; col < chroma_width; ++col) {
      uv_row[col * 2 + 0] = u_row[col];
      uv_row[col * 2 + 1] = v_row[col];
    }
  }
  buffer->SetCurrentLength(static_cast<DWORD>(frame_size));
  buffer->Unlock();

  hr = MFCreateSample(&out_sample);
  if (FAILED(hr)) {
    return hr;
  }
  hr = out_sample->AddBuffer(buffer.Get());
  if (FAILED(hr)) {
    return hr;
  }
  out_sample->SetSampleTime(timestamp);
  out_sample->SetSampleDuration(duration);
  return S_OK;
}

// HW H264 MFTs typically emit NV12; soft paths may offer RGB32/YUY2. Convert
// whatever we negotiated to non-premultiplied RGBA for VideoFrameRgba.
// `crop_*` select the clean aperture inside coded `width`×`height` (H264 is
// often macroblock-padded — showing the pad looks like a green right edge).
Roe<VideoFrameRgba> ConvertSampleToRgba(IMFSample* sample, int width, int height, const GUID& subtype,
                                         int crop_x, int crop_y, int visible_w, int visible_h) {
  ComPtr<IMFMediaBuffer> buffer;
  if (FAILED(sample->ConvertToContiguousBuffer(&buffer))) {
    return Error("decoder output buffer unavailable");
  }

  ComPtr<IMF2DBuffer2> buffer_2d;
  BYTE* scanline0 = nullptr;
  LONG pitch = 0;
  bool locked_2d = false;
  if (SUCCEEDED(buffer.As(&buffer_2d))) {
    BYTE* base = nullptr;
    DWORD length = 0;
    if (SUCCEEDED(buffer_2d->Lock2DSize(MF2DBuffer_LockFlags_Read, &scanline0, &pitch, &base,
                                         &length))) {
      locked_2d = true;
    }
  }
  BYTE* linear = nullptr;
  if (!locked_2d) {
    if (FAILED(buffer->Lock(&linear, nullptr, nullptr))) {
      return Error("decoder output lock failed");
    }
    scanline0 = linear;
    if (subtype == MFVideoFormat_NV12) {
      pitch = width;
    } else if (subtype == MFVideoFormat_YUY2) {
      pitch = width * 2;
    } else {
      pitch = width * 4;
    }
  }

  auto unlock = [&]() {
    if (locked_2d) {
      buffer_2d->Unlock2D();
    } else {
      buffer->Unlock();
    }
  };

  crop_x = std::max(0, crop_x);
  crop_y = std::max(0, crop_y);
  visible_w = (visible_w > 0) ? visible_w : width;
  visible_h = (visible_h > 0) ? visible_h : height;
  if (crop_x + visible_w > width) {
    visible_w = width - crop_x;
  }
  if (crop_y + visible_h > height) {
    visible_h = height - crop_y;
  }
  visible_w &= ~1;
  visible_h &= ~1;
  if (visible_w < 2 || visible_h < 2) {
    unlock();
    return Error("decoder visible aperture too small");
  }

  if (subtype == MFVideoFormat_NV12) {
    VideoFrameRgba full;
    if (!Nv12ToRgba(scanline0, width, height, pitch, full)) {
      unlock();
      return Error("NV12 decoder output convert failed");
    }
    unlock();
    if (crop_x == 0 && crop_y == 0 && visible_w == width && visible_h == height) {
      return full;
    }
    VideoFrameRgba out;
    out.width = visible_w;
    out.height = visible_h;
    out.rgba.resize(static_cast<size_t>(visible_w) * static_cast<size_t>(visible_h) * 4);
    for (int y = 0; y < visible_h; ++y) {
      const uint8_t* src = full.rgba.data() +
                           (static_cast<size_t>(crop_y + y) * static_cast<size_t>(width) +
                            static_cast<size_t>(crop_x)) *
                               4;
      uint8_t* dst = out.rgba.data() + static_cast<size_t>(y) * static_cast<size_t>(visible_w) * 4;
      std::memcpy(dst, src, static_cast<size_t>(visible_w) * 4);
    }
    return out;
  }
  if (subtype == MFVideoFormat_YUY2) {
    // Convert only the visible rows/cols via a tight sub-rectangle copy path:
    // build a contiguous NV12-like view is awkward for YUY2; convert full then crop.
    VideoFrameRgba full;
    if (!Yuy2ToRgba(scanline0, width, height, pitch, full)) {
      unlock();
      return Error("YUY2 decoder output convert failed");
    }
    unlock();
    if (crop_x == 0 && crop_y == 0 && visible_w == width && visible_h == height) {
      return full;
    }
    VideoFrameRgba out;
    out.width = visible_w;
    out.height = visible_h;
    out.rgba.resize(static_cast<size_t>(visible_w) * static_cast<size_t>(visible_h) * 4);
    for (int y = 0; y < visible_h; ++y) {
      const uint8_t* src = full.rgba.data() +
                           (static_cast<size_t>(crop_y + y) * static_cast<size_t>(width) +
                            static_cast<size_t>(crop_x)) *
                               4;
      uint8_t* dst = out.rgba.data() + static_cast<size_t>(y) * static_cast<size_t>(visible_w) * 4;
      std::memcpy(dst, src, static_cast<size_t>(visible_w) * 4);
    }
    return out;
  }

  // RGB32: packed B,G,R,X per pixel.
  VideoFrameRgba out;
  out.width = visible_w;
  out.height = visible_h;
  out.rgba.resize(static_cast<size_t>(visible_w) * static_cast<size_t>(visible_h) * 4);
  for (int y = 0; y < visible_h; ++y) {
    const uint8_t* src =
        scanline0 + static_cast<ptrdiff_t>(crop_y + y) * pitch + static_cast<ptrdiff_t>(crop_x) * 4;
    uint8_t* dst = out.rgba.data() + static_cast<size_t>(y) * static_cast<size_t>(visible_w) * 4;
    for (int x = 0; x < visible_w; ++x) {
      dst[x * 4 + 0] = src[x * 4 + 2];
      dst[x * 4 + 1] = src[x * 4 + 1];
      dst[x * 4 + 2] = src[x * 4 + 0];
      dst[x * 4 + 3] = 255;
    }
  }
  unlock();
  return out;
}

bool ReadDisplayAperture(IMFMediaType* type, int frame_w, int frame_h, int& crop_x, int& crop_y,
                         int& visible_w, int& visible_h) {
  crop_x = 0;
  crop_y = 0;
  visible_w = frame_w;
  visible_h = frame_h;
  if (!type || frame_w <= 0 || frame_h <= 0) {
    return false;
  }

  const GUID aperture_keys[] = {MF_MT_MINIMUM_DISPLAY_APERTURE, MF_MT_GEOMETRIC_APERTURE};
  for (const GUID& key : aperture_keys) {
    UINT32 blob_size = 0;
    if (FAILED(type->GetBlobSize(key, &blob_size)) || blob_size < sizeof(MFVideoArea)) {
      continue;
    }
    MFVideoArea area{};
    if (FAILED(type->GetBlob(key, reinterpret_cast<UINT8*>(&area), sizeof(area), &blob_size))) {
      continue;
    }
    const int x = static_cast<int>(area.OffsetX.value);
    const int y = static_cast<int>(area.OffsetY.value);
    const int w = static_cast<int>(area.Area.cx);
    const int h = static_cast<int>(area.Area.cy);
    if (w < 2 || h < 2 || x < 0 || y < 0 || x + w > frame_w || y + h > frame_h) {
      continue;
    }
    crop_x = x;
    crop_y = y;
    visible_w = w & ~1;
    visible_h = h & ~1;
    return visible_w >= 2 && visible_h >= 2;
  }
  return false;
}

class MediaFoundationVideoCodec final : public IVideoCodec {
public:
  MediaFoundationVideoCodec() = default;
  ~MediaFoundationVideoCodec() override;

  bool InitializePlatform(std::string& reason);

  std::string BackendName() const override { return "mediafoundation"; }
  bool HasEncoder() const override { return encoder_ != nullptr; }
  bool HasDecoder() const override { return decoder_ != nullptr; }

  Roe<void> ConfigureEncoder(int width, int height, int fps) override;
  Roe<void> ConfigureDecoder() override;

  Roe<EncodedAccessUnit> Encode(const VideoFrameI420& frame, bool force_keyframe) override;
  Roe<VideoFrameRgba> Decode(const uint8_t* annex_b, size_t size) override;

  void ResetEncoder() override;
  void ResetDecoder() override;

private:
  bool RenegotiateDecoderOutputType();

  bool com_initialized_ = false;
  bool mf_started_ = false;

  ComPtr<IMFTransform> encoder_;
  ComPtr<ICodecAPI> encoder_codec_api_;
  ComPtr<IMFMediaEventGenerator> encoder_events_;
  bool encoder_is_async_ = false;
  bool encoder_provides_samples_ = false;
  DWORD encoder_output_buffer_size_ = 0;
  int enc_width_ = 0;
  int enc_height_ = 0;
  int enc_fps_ = 0;
  LONGLONG enc_frame_duration_ = 0;
  LONGLONG enc_timestamp_ = 0;
  bool enc_first_frame_ = true;
  std::vector<uint8_t> enc_param_sets_;

  ComPtr<IMFTransform> decoder_;
  ComPtr<IMFMediaEventGenerator> decoder_events_;
  bool decoder_is_async_ = false;
  bool decoder_provides_samples_ = false;
  DWORD decoder_output_buffer_size_ = 0;
  GUID decode_subtype_ = MFVideoFormat_RGB32;
  int decode_width_ = 0;
  int decode_height_ = 0;
  int decode_crop_x_ = 0;
  int decode_crop_y_ = 0;
  int decode_visible_w_ = 0;
  int decode_visible_h_ = 0;
  LONGLONG decode_timestamp_ = 0;
};

MediaFoundationVideoCodec::~MediaFoundationVideoCodec() {
  ResetEncoder();
  ResetDecoder();
  if (mf_started_) {
    MFShutdown();
  }
  if (com_initialized_) {
    CoUninitialize();
  }
}

bool MediaFoundationVideoCodec::InitializePlatform(std::string& reason) {
  HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  if (hr == RPC_E_CHANGED_MODE) {
    hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  }
  if (FAILED(hr)) {
    reason = "COM initialization failed";
    return false;
  }
  com_initialized_ = true;

  hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
  if (FAILED(hr)) {
    reason = "Media Foundation startup failed";
    return false;
  }
  mf_started_ = true;
  return true;
}

Roe<void> MediaFoundationVideoCodec::ConfigureEncoder(int width, int height, int fps) {
  ResetEncoder();
  if (width <= 0 || height <= 0 || fps <= 0) {
    return Error("invalid H264 encoder configuration");
  }

  MFT_REGISTER_TYPE_INFO input_info{MFMediaType_Video, MFVideoFormat_NV12};
  MFT_REGISTER_TYPE_INFO output_info{MFMediaType_Video, MFVideoFormat_H264};
  auto candidates = EnumerateTransforms(MFT_CATEGORY_VIDEO_ENCODER, &input_info, &output_info);
  if (candidates.empty()) {
    return Error("no H264 encoder MFT available");
  }

  const UINT32 bitrate = EstimateBitrateBps(width, height, fps);

  for (auto& candidate : candidates) {
    const bool is_async = UnlockAsyncIfNeeded(candidate.Get());

    ComPtr<IMFMediaType> output_type;
    if (FAILED(MFCreateMediaType(&output_type)) ||
        FAILED(output_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video)) ||
        FAILED(output_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264)) ||
        FAILED(output_type->SetUINT32(MF_MT_AVG_BITRATE, bitrate)) ||
        FAILED(output_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive)) ||
        FAILED(output_type->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_ConstrainedBase)) ||
        FAILED(output_type->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE)) ||
        FAILED(MFSetAttributeSize(output_type.Get(), MF_MT_FRAME_SIZE, width, height)) ||
        FAILED(MFSetAttributeRatio(output_type.Get(), MF_MT_FRAME_RATE, fps, 1)) ||
        FAILED(MFSetAttributeRatio(output_type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1)) ||
        FAILED(candidate->SetOutputType(0, output_type.Get(), 0))) {
      continue;
    }

    ComPtr<IMFMediaType> input_type;
    if (FAILED(MFCreateMediaType(&input_type)) ||
        FAILED(input_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video)) ||
        FAILED(input_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12)) ||
        FAILED(input_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive)) ||
        FAILED(MFSetAttributeSize(input_type.Get(), MF_MT_FRAME_SIZE, width, height)) ||
        FAILED(MFSetAttributeRatio(input_type.Get(), MF_MT_FRAME_RATE, fps, 1)) ||
        FAILED(MFSetAttributeRatio(input_type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1)) ||
        FAILED(candidate->SetInputType(0, input_type.Get(), 0))) {
      continue;
    }

    ComPtr<ICodecAPI> codec_api;
    if (SUCCEEDED(candidate.As(&codec_api)) && codec_api) {
      VARIANT rate_mode;
      VariantInit(&rate_mode);
      rate_mode.vt = VT_UI4;
      rate_mode.ulVal = eAVEncCommonRateControlMode_CBR;
      codec_api->SetValue(&CODECAPI_AVEncCommonRateControlMode, &rate_mode);

      VARIANT mean_bitrate;
      VariantInit(&mean_bitrate);
      mean_bitrate.vt = VT_UI4;
      mean_bitrate.ulVal = bitrate;
      codec_api->SetValue(&CODECAPI_AVEncCommonMeanBitRate, &mean_bitrate);

      VARIANT low_latency;
      VariantInit(&low_latency);
      low_latency.vt = VT_BOOL;
      low_latency.boolVal = VARIANT_TRUE;
      codec_api->SetValue(&CODECAPI_AVLowLatencyMode, &low_latency);
    }

    MFT_OUTPUT_STREAM_INFO stream_info{};
    if (FAILED(candidate->GetOutputStreamInfo(0, &stream_info))) {
      continue;
    }

    candidate->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    candidate->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    candidate->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

    encoder_ = candidate;
    encoder_codec_api_ = codec_api;
    encoder_is_async_ = is_async;
    encoder_events_.Reset();
    if (is_async) {
      encoder_.As(&encoder_events_);
    }
    encoder_provides_samples_ = (stream_info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) != 0;
    encoder_output_buffer_size_ =
        stream_info.cbSize ? stream_info.cbSize
                            : static_cast<DWORD>(width) * static_cast<DWORD>(height);
    enc_width_ = width;
    enc_height_ = height;
    enc_fps_ = fps;
    enc_frame_duration_ = 10000000LL / fps;
    enc_timestamp_ = 0;
    enc_first_frame_ = true;
    enc_param_sets_.clear();
    {
      ComPtr<IMFMediaType> current_output;
      if (SUCCEEDED(encoder_->GetOutputCurrentType(0, &current_output)) && current_output) {
        UINT32 blob_size = 0;
        if (SUCCEEDED(current_output->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &blob_size)) &&
            blob_size > 0) {
          enc_param_sets_.resize(blob_size);
          if (FAILED(current_output->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER, enc_param_sets_.data(),
                                              blob_size, &blob_size))) {
            enc_param_sets_.clear();
          } else {
            enc_param_sets_.resize(blob_size);
          }
        }
      }
    }
    return {};
  }

  return Error("failed to negotiate H264 encoder media types");
}

Roe<void> MediaFoundationVideoCodec::ConfigureDecoder() {
  ResetDecoder();

  MFT_REGISTER_TYPE_INFO input_info{MFMediaType_Video, MFVideoFormat_H264};
  // Prefer sync software decoder for receive: HW/DXVA paths often need a D3D
  // device we do not bind, and fail NV12/RGB32 renegotiation silently.
  auto candidates = EnumerateTransforms(MFT_CATEGORY_VIDEO_DECODER, &input_info, nullptr,
                                         /*prefer_hardware=*/false);
  if (candidates.empty()) {
    return Error("no H264 decoder MFT available");
  }

  for (auto& candidate : candidates) {
    const bool is_async = UnlockAsyncIfNeeded(candidate.Get());

    ComPtr<IMFMediaType> input_type;
    if (FAILED(MFCreateMediaType(&input_type)) ||
        FAILED(input_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video)) ||
        FAILED(input_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264)) ||
        FAILED(input_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive)) ||
        FAILED(MFSetAttributeSize(input_type.Get(), MF_MT_FRAME_SIZE, kDefaultDecodeWidth,
                                   kDefaultDecodeHeight)) ||
        FAILED(MFSetAttributeRatio(input_type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1)) ||
        FAILED(candidate->SetInputType(0, input_type.Get(), 0))) {
      continue;
    }

    candidate->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    candidate->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    candidate->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

    decoder_ = candidate;
    decoder_is_async_ = is_async;
    decoder_events_.Reset();
    if (is_async) {
      decoder_.As(&decoder_events_);
    }
    decoder_provides_samples_ = false;
    decoder_output_buffer_size_ =
        static_cast<DWORD>(kDefaultDecodeWidth) * static_cast<DWORD>(kDefaultDecodeHeight) * 2;
    decode_width_ = kDefaultDecodeWidth;
    decode_height_ = kDefaultDecodeHeight;
    decode_subtype_ = MFVideoFormat_NV12;
    decode_crop_x_ = 0;
    decode_crop_y_ = 0;
    decode_visible_w_ = decode_width_;
    decode_visible_h_ = decode_height_;
    decode_timestamp_ = 0;
    // Best-effort provisional NV12 output so the first ProcessOutput is less
    // likely to return TYPE_NOT_SET. SPS may still trigger STREAM_CHANGE.
    (void)RenegotiateDecoderOutputType();
    return {};
  }

  return Error("failed to negotiate H264 decoder input type");
}

bool MediaFoundationVideoCodec::RenegotiateDecoderOutputType() {
  // H264 MFTs typically expose NV12 first; RGB32 is a soft-MFT convenience.
  const GUID preference[] = {MFVideoFormat_NV12, MFVideoFormat_RGB32, MFVideoFormat_YUY2};
  for (const GUID& wanted : preference) {
    for (DWORD index = 0;; ++index) {
      ComPtr<IMFMediaType> candidate;
      const HRESULT hr = decoder_->GetOutputAvailableType(0, index, &candidate);
      if (hr == MF_E_NO_MORE_TYPES || FAILED(hr)) {
        break;
      }
      GUID subtype{};
      if (FAILED(candidate->GetGUID(MF_MT_SUBTYPE, &subtype)) || subtype != wanted) {
        continue;
      }
      if (FAILED(decoder_->SetOutputType(0, candidate.Get(), 0))) {
        continue;
      }
      UINT32 width = 0;
      UINT32 height = 0;
      if (SUCCEEDED(MFGetAttributeSize(candidate.Get(), MF_MT_FRAME_SIZE, &width, &height)) &&
          width && height) {
        decode_width_ = static_cast<int>(width);
        decode_height_ = static_cast<int>(height);
      }
      decode_crop_x_ = 0;
      decode_crop_y_ = 0;
      decode_visible_w_ = decode_width_;
      decode_visible_h_ = decode_height_;
      (void)ReadDisplayAperture(candidate.Get(), decode_width_, decode_height_, decode_crop_x_,
                                decode_crop_y_, decode_visible_w_, decode_visible_h_);
      MFT_OUTPUT_STREAM_INFO stream_info{};
      decoder_->GetOutputStreamInfo(0, &stream_info);
      decoder_provides_samples_ = (stream_info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) != 0;
      const DWORD bytes_per_pixel = (wanted == MFVideoFormat_NV12) ? 2
                                    : (wanted == MFVideoFormat_YUY2) ? 2 : 4;
      decoder_output_buffer_size_ =
          stream_info.cbSize
              ? stream_info.cbSize
              : static_cast<DWORD>(decode_width_) * static_cast<DWORD>(decode_height_) *
                    bytes_per_pixel;
      decode_subtype_ = wanted;
      return true;
    }
  }
  return false;
}

Roe<EncodedAccessUnit> MediaFoundationVideoCodec::Encode(const VideoFrameI420& frame,
                                                          bool force_keyframe) {
  if (!encoder_) {
    return Error("H264 encoder not configured");
  }
  if (frame.width != enc_width_ || frame.height != enc_height_) {
    return Error("encoder frame size does not match ConfigureEncoder size");
  }

  if (force_keyframe && encoder_codec_api_) {
    VARIANT force_key;
    VariantInit(&force_key);
    force_key.vt = VT_UI4;
    force_key.ulVal = TRUE;
    encoder_codec_api_->SetValue(&CODECAPI_AVEncVideoForceKeyFrame, &force_key);
  }

  ComPtr<IMFSample> input_sample;
  const LONGLONG timestamp = enc_timestamp_;
  enc_timestamp_ += enc_frame_duration_;
  if (FAILED(BuildNv12Sample(frame, timestamp, enc_frame_duration_, input_sample))) {
    return Error("failed to build NV12 sample for encoder");
  }

  PumpOutcome outcome = PumpTransform(encoder_.Get(), encoder_events_.Get(), encoder_is_async_, 0,
                                       encoder_output_buffer_size_, encoder_provides_samples_,
                                       input_sample.Get());
  if (outcome.error) {
    return Error("H264 encode failed");
  }
  if (outcome.stream_change) {
    return Error("unexpected H264 encoder stream change");
  }

  EncodedAccessUnit unit;
  bool keyframe = false;
  bool clean_point_seen = false;
  for (auto& sample : outcome.samples) {
    UINT32 clean_point = 0;
    if (SUCCEEDED(sample->GetUINT32(MFSampleExtension_CleanPoint, &clean_point))) {
      clean_point_seen = true;
      keyframe = keyframe || (clean_point != 0);
    }
    AppendSampleBytes(sample.Get(), unit.annex_b);
  }
  if (!clean_point_seen) {
    keyframe = force_keyframe || enc_first_frame_;
  }
  enc_first_frame_ = false;
  unit.keyframe = keyframe;

  // Cache SPS/PPS from the bitstream when the output type did not expose them,
  // and ensure every keyframe carries them so Android MediaCodec can configure.
  if (!unit.annex_b.empty()) {
    auto extract_param_sets = [](const std::vector<uint8_t>& bytes) {
      std::vector<uint8_t> params;
      size_t i = 0;
      while (i + 3 < bytes.size()) {
        size_t sc = 0;
        if (bytes[i] == 0 && bytes[i + 1] == 0 && bytes[i + 2] == 1) {
          sc = 3;
        } else if (i + 4 < bytes.size() && bytes[i] == 0 && bytes[i + 1] == 0 && bytes[i + 2] == 0 &&
                   bytes[i + 3] == 1) {
          sc = 4;
        } else {
          ++i;
          continue;
        }
        const size_t nal_start = i + sc;
        if (nal_start >= bytes.size()) {
          break;
        }
        size_t nal_end = bytes.size();
        for (size_t j = nal_start + 1; j + 3 < bytes.size(); ++j) {
          if (bytes[j] == 0 && bytes[j + 1] == 0 &&
              (bytes[j + 2] == 1 || (j + 4 <= bytes.size() && bytes[j + 2] == 0 && bytes[j + 3] == 1))) {
            nal_end = j;
            break;
          }
        }
        const int nal_type = bytes[nal_start] & 0x1F;
        if (nal_type == 7 || nal_type == 8) {
          static const uint8_t kStart[4] = {0, 0, 0, 1};
          params.insert(params.end(), kStart, kStart + 4);
          params.insert(params.end(), bytes.begin() + static_cast<std::ptrdiff_t>(nal_start),
                        bytes.begin() + static_cast<std::ptrdiff_t>(nal_end));
        }
        i = nal_end;
      }
      return params;
    };
    auto params = extract_param_sets(unit.annex_b);
    if (!params.empty()) {
      enc_param_sets_ = std::move(params);
    } else if (keyframe && !enc_param_sets_.empty()) {
      std::vector<uint8_t> with_params = enc_param_sets_;
      with_params.insert(with_params.end(), unit.annex_b.begin(), unit.annex_b.end());
      unit.annex_b = std::move(with_params);
    }
  }
  return unit;
}

Roe<VideoFrameRgba> MediaFoundationVideoCodec::Decode(const uint8_t* annex_b, size_t size) {
  if (!decoder_) {
    return Error("H264 decoder not configured");
  }
  if (!annex_b || size == 0) {
    return Error("empty H264 decoder input");
  }

  ComPtr<IMFMediaBuffer> buffer;
  if (FAILED(MFCreateMemoryBuffer(static_cast<DWORD>(size), &buffer))) {
    return Error("failed to allocate decoder input buffer");
  }
  BYTE* dst = nullptr;
  if (FAILED(buffer->Lock(&dst, nullptr, nullptr))) {
    return Error("failed to lock decoder input buffer");
  }
  std::memcpy(dst, annex_b, size);
  buffer->SetCurrentLength(static_cast<DWORD>(size));
  buffer->Unlock();

  ComPtr<IMFSample> input_sample;
  if (FAILED(MFCreateSample(&input_sample)) || FAILED(input_sample->AddBuffer(buffer.Get()))) {
    return Error("failed to build decoder input sample");
  }
  input_sample->SetSampleTime(decode_timestamp_);
  decode_timestamp_ += 333333LL; // ~30fps hint; renderer paces on wall clock, not this pts.
  if (AnnexBContainsIdr(annex_b, size)) {
    input_sample->SetUINT32(MFSampleExtension_CleanPoint, TRUE);
  }

  PumpOutcome outcome = PumpTransform(decoder_.Get(), decoder_events_.Get(), decoder_is_async_, 0,
                                       decoder_output_buffer_size_, decoder_provides_samples_,
                                       input_sample.Get());
  if (outcome.error) {
    char buf[96];
    std::snprintf(buf, sizeof(buf), "H264 decode failed hr=0x%08lX",
                  static_cast<unsigned long>(outcome.hr));
    return Error(buf);
  }
  if (outcome.stream_change) {
    // MF contract: after STREAM_CHANGE / TYPE_NOT_SET, SetOutputType then
    // ProcessOutput again. Soft H264 often still returns NEED_MORE_INPUT for
    // TYPE_NOT_SET — re-feed the same AU once the output type exists.
    for (int attempt = 0; attempt < 4; ++attempt) {
      if (!RenegotiateDecoderOutputType()) {
        return Error("H264 decoder output type negotiation failed");
      }
      ComPtr<IMFSample> sample;
      const HRESULT retry_hr = ProcessOneOutput(decoder_.Get(), 0, decoder_output_buffer_size_,
                                                 decoder_provides_samples_, sample);
      if (retry_hr == MF_E_TRANSFORM_STREAM_CHANGE || retry_hr == MF_E_TRANSFORM_TYPE_NOT_SET) {
        continue;
      }
      if (SUCCEEDED(retry_hr)) {
        return ConvertSampleToRgba(sample.Get(), decode_width_, decode_height_, decode_subtype_,
                                   decode_crop_x_, decode_crop_y_, decode_visible_w_,
                                   decode_visible_h_);
      }
      if (retry_hr != MF_E_TRANSFORM_NEED_MORE_INPUT) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "H264 decode failed after stream change hr=0x%08lX",
                      static_cast<unsigned long>(retry_hr));
        return Error(buf);
      }
      break;
    }

    // Rebuild a fresh sample — MFTs often reject re-submitting a buffer that
    // was already accepted by ProcessInput during the TYPE_NOT_SET pass.
    ComPtr<IMFMediaBuffer> replay_buffer;
    if (FAILED(MFCreateMemoryBuffer(static_cast<DWORD>(size), &replay_buffer))) {
      return Error("failed to allocate decoder replay buffer");
    }
    BYTE* replay_dst = nullptr;
    if (FAILED(replay_buffer->Lock(&replay_dst, nullptr, nullptr))) {
      return Error("failed to lock decoder replay buffer");
    }
    std::memcpy(replay_dst, annex_b, size);
    replay_buffer->SetCurrentLength(static_cast<DWORD>(size));
    replay_buffer->Unlock();
    ComPtr<IMFSample> replay_sample;
    if (FAILED(MFCreateSample(&replay_sample)) ||
        FAILED(replay_sample->AddBuffer(replay_buffer.Get()))) {
      return Error("failed to build decoder replay sample");
    }
    replay_sample->SetSampleTime(decode_timestamp_);
    if (AnnexBContainsIdr(annex_b, size)) {
      replay_sample->SetUINT32(MFSampleExtension_CleanPoint, TRUE);
    }

    PumpOutcome replay =
        PumpTransform(decoder_.Get(), decoder_events_.Get(), decoder_is_async_, 0,
                      decoder_output_buffer_size_, decoder_provides_samples_, replay_sample.Get());
    if (replay.error) {
      char buf[96];
      std::snprintf(buf, sizeof(buf), "H264 decode replay failed hr=0x%08lX",
                    static_cast<unsigned long>(replay.hr));
      return Error(buf);
    }
    if (replay.stream_change) {
      if (!RenegotiateDecoderOutputType()) {
        return Error("H264 decoder output type negotiation failed on replay");
      }
      ComPtr<IMFSample> sample;
      const HRESULT retry_hr = ProcessOneOutput(decoder_.Get(), 0, decoder_output_buffer_size_,
                                                 decoder_provides_samples_, sample);
      if (SUCCEEDED(retry_hr)) {
        return ConvertSampleToRgba(sample.Get(), decode_width_, decode_height_, decode_subtype_,
                                   decode_crop_x_, decode_crop_y_, decode_visible_w_,
                                   decode_visible_h_);
      }
      // Fall through — next remote AU should decode with the type locked in.
      return Error("decoder needs more input after format change");
    }
    if (replay.samples.empty()) {
      return Error("decoder needs more input after format change");
    }
    return ConvertSampleToRgba(replay.samples.back().Get(), decode_width_, decode_height_,
                               decode_subtype_, decode_crop_x_, decode_crop_y_, decode_visible_w_,
                               decode_visible_h_);
  }
  if (outcome.samples.empty()) {
    return Error("decoder needs more input to produce a frame");
  }
  return ConvertSampleToRgba(outcome.samples.back().Get(), decode_width_, decode_height_,
                             decode_subtype_, decode_crop_x_, decode_crop_y_, decode_visible_w_,
                             decode_visible_h_);
}

void MediaFoundationVideoCodec::ResetEncoder() {
  if (encoder_) {
    encoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
    encoder_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
  }
  encoder_events_.Reset();
  encoder_codec_api_.Reset();
  encoder_.Reset();
  encoder_is_async_ = false;
  encoder_provides_samples_ = false;
  encoder_output_buffer_size_ = 0;
  enc_width_ = 0;
  enc_height_ = 0;
  enc_fps_ = 0;
  enc_frame_duration_ = 0;
  enc_timestamp_ = 0;
  enc_first_frame_ = true;
  enc_param_sets_.clear();
}

void MediaFoundationVideoCodec::ResetDecoder() {
  if (decoder_) {
    decoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
    decoder_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
  }
  decoder_events_.Reset();
  decoder_.Reset();
  decoder_is_async_ = false;
  decoder_provides_samples_ = false;
  decoder_output_buffer_size_ = 0;
  decode_subtype_ = MFVideoFormat_RGB32;
  decode_width_ = 0;
  decode_height_ = 0;
  decode_crop_x_ = 0;
  decode_crop_y_ = 0;
  decode_visible_w_ = 0;
  decode_visible_h_ = 0;
  decode_timestamp_ = 0;
}

} // namespace

std::unique_ptr<IVideoCodec> CreateOsVideoCodec() {
  auto codec = std::make_unique<MediaFoundationVideoCodec>();
  std::string reason;
  if (!codec->InitializePlatform(reason)) {
    return MakeUnavailableVideoCodec(reason.empty() ? "Media Foundation unavailable" : reason);
  }
  return codec;
}

} // namespace pbr

#endif // defined(_WIN32)
