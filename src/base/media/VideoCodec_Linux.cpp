#include "base/media/VideoCodecOs.h"
#include "base/media/VideoCodecUnavailable.h"
#include "base/media/VideoYuv.h"

#if defined(__linux__) && !defined(__ANDROID__)

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <string>
#include <unistd.h>
#include <vector>

#if defined(PP_BROWSER_HAS_LIBVA)

#include <va/va.h>
#include <va/va_drm.h>
#include <va/va_enc_h264.h>

namespace pbr {
namespace {

constexpr int64_t kMinBitrateBps = 200000;
constexpr int64_t kMaxBitrateBps = 4000000;
constexpr int kMaxDpbSurfaces = 4;
constexpr int kIntraIdrPeriod = 48; // force IDR periodically (~2s @ 24fps)

int32_t EstimateBitrateBps(int width, int height, int fps) {
  const double raw = static_cast<double>(width) * static_cast<double>(height) *
                      static_cast<double>(std::max(fps, 1)) * 0.07;
  const double clamped =
      std::clamp(raw, static_cast<double>(kMinBitrateBps), static_cast<double>(kMaxBitrateBps));
  return static_cast<int32_t>(clamped);
}

int Align16(int v) { return (v + 15) & ~15; }

void InvalidatePicture(VAPictureH264& pic) {
  pic.picture_id = VA_INVALID_ID;
  pic.frame_idx = 0;
  pic.flags = VA_PICTURE_H264_INVALID;
  pic.TopFieldOrderCnt = 0;
  pic.BottomFieldOrderCnt = 0;
}

// ---- Annex-B parsing -------------------------------------------------

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

std::vector<uint8_t> NalToRbsp(const uint8_t* data, size_t size) {
  std::vector<uint8_t> rbsp;
  rbsp.reserve(size);
  for (size_t i = 0; i < size; ++i) {
    if (i + 2 < size && data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 3) {
      rbsp.push_back(0);
      rbsp.push_back(0);
      i += 2;
      continue;
    }
    rbsp.push_back(data[i]);
  }
  return rbsp;
}

// ---- Bitstream writer (SPS/PPS packed headers) -----------------------

class BitWriter {
public:
  void PutBits(uint32_t value, int nbits) {
    for (int i = nbits - 1; i >= 0; --i) {
      cur_ = static_cast<uint8_t>((cur_ << 1) | ((value >> i) & 1));
      ++bits_;
      if (bits_ == 8) {
        FlushByte();
      }
    }
  }
  void PutUe(uint32_t v) {
    ++v;
    int bits = 0;
    for (uint32_t t = v; t; t >>= 1) {
      ++bits;
    }
    PutBits(0, bits - 1);
    PutBits(v, bits);
  }
  void PutSe(int v) {
    const uint32_t mapped = v <= 0 ? static_cast<uint32_t>(-2 * v) : static_cast<uint32_t>(2 * v - 1);
    PutUe(mapped);
  }
  void RbspTrailingBits() {
    PutBits(1, 1);
    while (bits_ != 0) {
      PutBits(0, 1);
    }
  }
  const std::vector<uint8_t>& Bytes() const { return bytes_; }

private:
  void FlushByte() {
    bytes_.push_back(cur_);
    cur_ = 0;
    bits_ = 0;
  }
  std::vector<uint8_t> bytes_;
  uint8_t cur_ = 0;
  int bits_ = 0;
};

// ---- Bitstream reader (decode SPS/PPS/slice header) ------------------

class BitReader {
public:
  BitReader(const uint8_t* data, size_t size) : data_(data), size_(size) {}

  bool Ok() const { return ok_; }
  size_t BitsRead() const { return bits_read_; }

  uint32_t ReadBits(int n) {
    uint32_t v = 0;
    for (int i = 0; i < n; ++i) {
      v = (v << 1) | ReadBit();
    }
    return v;
  }
  uint32_t ReadUe() {
    int zeros = 0;
    while (Ok() && ReadBit() == 0) {
      ++zeros;
      if (zeros > 31) {
        ok_ = false;
        return 0;
      }
    }
    if (!Ok()) {
      return 0;
    }
    uint32_t v = 1;
    if (zeros) {
      v = (1u << zeros) | ReadBits(zeros);
    }
    return v - 1;
  }
  int32_t ReadSe() {
    const uint32_t v = ReadUe();
    if ((v & 1) == 0) {
      return -static_cast<int32_t>(v / 2);
    }
    return static_cast<int32_t>((v + 1) / 2);
  }
  void SkipBits(int n) { (void)ReadBits(n); }

private:
  uint32_t ReadBit() {
    if (byte_pos_ >= size_) {
      ok_ = false;
      return 0;
    }
    const uint32_t bit = (data_[byte_pos_] >> (7 - bit_pos_)) & 1u;
    ++bits_read_;
    if (++bit_pos_ == 8) {
      bit_pos_ = 0;
      ++byte_pos_;
    }
    return bit;
  }

  const uint8_t* data_ = nullptr;
  size_t size_ = 0;
  size_t byte_pos_ = 0;
  int bit_pos_ = 0;
  size_t bits_read_ = 0;
  bool ok_ = true;
};

struct ParsedSps {
  int profile_idc = 66;
  int level_idc = 31;
  int chroma_format_idc = 1;
  int bit_depth_luma_minus8 = 0;
  int bit_depth_chroma_minus8 = 0;
  int log2_max_frame_num_minus4 = 0;
  int pic_order_cnt_type = 0;
  int log2_max_pic_order_cnt_lsb_minus4 = 0;
  int delta_pic_order_always_zero_flag = 0;
  int max_num_ref_frames = 1;
  int pic_width_in_mbs_minus1 = 0;
  int pic_height_in_map_units_minus1 = 0;
  int frame_mbs_only_flag = 1;
  int direct_8x8_inference_flag = 1;
  int frame_cropping_flag = 0;
  int crop_left = 0;
  int crop_right = 0;
  int crop_top = 0;
  int crop_bottom = 0;
  int coded_width = 0;
  int coded_height = 0;
  int width = 0;
  int height = 0;
};

struct ParsedPps {
  int pic_parameter_set_id = 0;
  int seq_parameter_set_id = 0;
  int entropy_coding_mode_flag = 0;
  int bottom_field_pic_order_in_frame_present_flag = 0;
  int num_ref_idx_l0_default_active_minus1 = 0;
  int num_ref_idx_l1_default_active_minus1 = 0;
  int weighted_pred_flag = 0;
  int weighted_bipred_idc = 0;
  int pic_init_qp_minus26 = 0;
  int chroma_qp_index_offset = 0;
  int deblocking_filter_control_present_flag = 1;
  int constrained_intra_pred_flag = 0;
  int redundant_pic_cnt_present_flag = 0;
  int transform_8x8_mode_flag = 0;
  int second_chroma_qp_index_offset = 0;
};

bool ParseSpsRbsp(const uint8_t* rbsp, size_t size, ParsedSps& out) {
  if (size < 4) {
    return false;
  }
  BitReader br(rbsp + 1, size - 1); // skip NAL header
  out.profile_idc = static_cast<int>(br.ReadBits(8));
  br.SkipBits(8); // constraint flags + reserved
  out.level_idc = static_cast<int>(br.ReadBits(8));
  (void)br.ReadUe(); // sps id
  out.chroma_format_idc = 1;
  if (out.profile_idc == 100 || out.profile_idc == 110 || out.profile_idc == 122 ||
      out.profile_idc == 244 || out.profile_idc == 44 || out.profile_idc == 83 ||
      out.profile_idc == 86 || out.profile_idc == 118 || out.profile_idc == 128 ||
      out.profile_idc == 138 || out.profile_idc == 139 || out.profile_idc == 134) {
    out.chroma_format_idc = static_cast<int>(br.ReadUe());
    if (out.chroma_format_idc == 3) {
      br.SkipBits(1);
    }
    out.bit_depth_luma_minus8 = static_cast<int>(br.ReadUe());
    out.bit_depth_chroma_minus8 = static_cast<int>(br.ReadUe());
    br.SkipBits(1); // qpprime_y_zero_transform_bypass_flag
    if (br.ReadBits(1)) {
      return false; // scaling matrix not handled
    }
  }
  out.log2_max_frame_num_minus4 = static_cast<int>(br.ReadUe());
  out.pic_order_cnt_type = static_cast<int>(br.ReadUe());
  out.delta_pic_order_always_zero_flag = 0;
  if (out.pic_order_cnt_type == 0) {
    out.log2_max_pic_order_cnt_lsb_minus4 = static_cast<int>(br.ReadUe());
  } else if (out.pic_order_cnt_type == 1) {
    out.delta_pic_order_always_zero_flag = static_cast<int>(br.ReadBits(1));
    (void)br.ReadSe();
    (void)br.ReadSe();
    const uint32_t n = br.ReadUe();
    for (uint32_t i = 0; i < n; ++i) {
      (void)br.ReadSe();
    }
  }
  out.max_num_ref_frames = static_cast<int>(br.ReadUe());
  br.SkipBits(1); // gaps
  out.pic_width_in_mbs_minus1 = static_cast<int>(br.ReadUe());
  out.pic_height_in_map_units_minus1 = static_cast<int>(br.ReadUe());
  out.frame_mbs_only_flag = static_cast<int>(br.ReadBits(1));
  if (!out.frame_mbs_only_flag) {
    br.SkipBits(1);
  }
  out.direct_8x8_inference_flag = static_cast<int>(br.ReadBits(1));
  out.frame_cropping_flag = static_cast<int>(br.ReadBits(1));
  if (out.frame_cropping_flag) {
    out.crop_left = static_cast<int>(br.ReadUe());
    out.crop_right = static_cast<int>(br.ReadUe());
    out.crop_top = static_cast<int>(br.ReadUe());
    out.crop_bottom = static_cast<int>(br.ReadUe());
  }
  if (!br.Ok()) {
    return false;
  }
  const int width_mbs = out.pic_width_in_mbs_minus1 + 1;
  const int height_mbs = (out.pic_height_in_map_units_minus1 + 1) * (2 - out.frame_mbs_only_flag);
  out.coded_width = width_mbs * 16;
  out.coded_height = height_mbs * 16;
  out.width = out.coded_width;
  out.height = out.coded_height;
  if (out.frame_cropping_flag) {
    const int crop_unit_x = (out.chroma_format_idc == 1 || out.chroma_format_idc == 2) ? 2 : 1;
    const int crop_unit_y =
        (out.chroma_format_idc == 1) ? (2 * (2 - out.frame_mbs_only_flag)) : (2 - out.frame_mbs_only_flag);
    out.width -= (out.crop_left + out.crop_right) * crop_unit_x;
    out.height -= (out.crop_top + out.crop_bottom) * crop_unit_y;
  }
  return out.width > 0 && out.height > 0 && out.coded_width > 0 && out.coded_height > 0;
}

bool ParsePpsRbsp(const uint8_t* rbsp, size_t size, ParsedPps& out) {
  if (size < 2) {
    return false;
  }
  BitReader br(rbsp + 1, size - 1);
  out.pic_parameter_set_id = static_cast<int>(br.ReadUe());
  out.seq_parameter_set_id = static_cast<int>(br.ReadUe());
  out.entropy_coding_mode_flag = static_cast<int>(br.ReadBits(1));
  out.bottom_field_pic_order_in_frame_present_flag = static_cast<int>(br.ReadBits(1));
  if (br.ReadUe() != 0) {
    return false; // FMO unsupported
  }
  out.num_ref_idx_l0_default_active_minus1 = static_cast<int>(br.ReadUe());
  out.num_ref_idx_l1_default_active_minus1 = static_cast<int>(br.ReadUe());
  out.weighted_pred_flag = static_cast<int>(br.ReadBits(1));
  out.weighted_bipred_idc = static_cast<int>(br.ReadBits(2));
  out.pic_init_qp_minus26 = br.ReadSe();
  (void)br.ReadSe(); // qs
  out.chroma_qp_index_offset = br.ReadSe();
  out.deblocking_filter_control_present_flag = static_cast<int>(br.ReadBits(1));
  out.constrained_intra_pred_flag = static_cast<int>(br.ReadBits(1));
  out.redundant_pic_cnt_present_flag = static_cast<int>(br.ReadBits(1));
  if (!br.Ok()) {
    return false;
  }
  // Optional High-profile more_rbsp_data() tail. VideoToolbox Baseline PPS is often
  // 3 payload bytes ending in rbsp_trailing_bits (0x80). Comparing against the full NAL
  // size (including the header byte) falsely treated that trailing byte as
  // transform_8x8_mode_flag=1 → VA High profile + black frames with a tiny corner scrap.
  out.transform_8x8_mode_flag = 0;
  out.second_chroma_qp_index_offset = out.chroma_qp_index_offset;
  const size_t payload_size = size - 1;
  const size_t consumed_bytes = (br.BitsRead() + 7) / 8;
  if (consumed_bytes + 1 < payload_size) {
    out.transform_8x8_mode_flag = static_cast<int>(br.ReadBits(1));
    if (br.Ok() && br.ReadBits(1) == 0) {
      const int32_t second = br.ReadSe();
      if (br.Ok()) {
        out.second_chroma_qp_index_offset = second;
      }
    }
  }
  return true;
}

struct ParsedSliceHeader {
  int first_mb_in_slice = 0;
  int slice_type = 0;
  int pic_parameter_set_id = 0;
  int frame_num = 0;
  int idr_pic_id = 0;
  int pic_order_cnt_lsb = 0;
  int num_ref_idx_active_override_flag = 0;
  int num_ref_idx_l0_active_minus1 = 0;
  int num_ref_idx_l1_active_minus1 = 0;
  int cabac_init_idc = 0;
  int slice_qp_delta = 0;
  int disable_deblocking_filter_idc = 0;
  int slice_alpha_c0_offset_div2 = 0;
  int slice_beta_offset_div2 = 0;
  size_t header_bit_size = 0; // bits from start of RBSP (incl. NAL header)
  bool idr = false;
};

bool ParseSliceHeader(const uint8_t* rbsp, size_t size, const ParsedSps& sps, const ParsedPps& pps,
                      bool idr, ParsedSliceHeader& out) {
  if (size < 2) {
    return false;
  }
  BitReader br(rbsp, size);
  br.SkipBits(8); // NAL header
  out.idr = idr;
  out.first_mb_in_slice = static_cast<int>(br.ReadUe());
  out.slice_type = static_cast<int>(br.ReadUe()) % 5;
  out.pic_parameter_set_id = static_cast<int>(br.ReadUe());
  const int frame_num_bits = sps.log2_max_frame_num_minus4 + 4;
  out.frame_num = static_cast<int>(br.ReadBits(frame_num_bits));
  if (!sps.frame_mbs_only_flag) {
    if (br.ReadBits(1)) {
      br.SkipBits(1); // bottom_field_flag
    }
  }
  if (idr) {
    out.idr_pic_id = static_cast<int>(br.ReadUe());
  }
  if (sps.pic_order_cnt_type == 0) {
    const int poc_bits = sps.log2_max_pic_order_cnt_lsb_minus4 + 4;
    out.pic_order_cnt_lsb = static_cast<int>(br.ReadBits(poc_bits));
    if (pps.bottom_field_pic_order_in_frame_present_flag) {
      (void)br.ReadSe();
    }
  } else if (sps.pic_order_cnt_type == 1 && sps.delta_pic_order_always_zero_flag == 0) {
    (void)br.ReadSe();
    if (pps.bottom_field_pic_order_in_frame_present_flag) {
      (void)br.ReadSe();
    }
  }
  if (pps.redundant_pic_cnt_present_flag) {
    (void)br.ReadUe();
  }
  if (out.slice_type == 1) { // B
    br.SkipBits(1);
  }
  out.num_ref_idx_l0_active_minus1 = pps.num_ref_idx_l0_default_active_minus1;
  out.num_ref_idx_l1_active_minus1 = pps.num_ref_idx_l1_default_active_minus1;
  if (out.slice_type == 0 || out.slice_type == 1) { // P or B
    out.num_ref_idx_active_override_flag = static_cast<int>(br.ReadBits(1));
    if (out.num_ref_idx_active_override_flag) {
      out.num_ref_idx_l0_active_minus1 = static_cast<int>(br.ReadUe());
      if (out.slice_type == 1) {
        out.num_ref_idx_l1_active_minus1 = static_cast<int>(br.ReadUe());
      }
    }
    // ref_pic_list_modification — only modification_flag
    if (br.ReadBits(1)) {
      for (;;) {
        const uint32_t idc = br.ReadUe();
        if (idc == 3) {
          break;
        }
        if (idc == 0 || idc == 1) {
          (void)br.ReadUe();
        } else if (idc == 2) {
          (void)br.ReadUe();
        } else {
          return false;
        }
      }
    }
    if (out.slice_type == 1) {
      if (br.ReadBits(1)) {
        for (;;) {
          const uint32_t idc = br.ReadUe();
          if (idc == 3) {
            break;
          }
          if (idc == 0 || idc == 1 || idc == 2) {
            (void)br.ReadUe();
          } else {
            return false;
          }
        }
      }
    }
  }
  if ((pps.weighted_pred_flag && out.slice_type == 0) ||
      (pps.weighted_bipred_idc == 1 && out.slice_type == 1)) {
    return false; // weighted pred not handled
  }
  if (idr) {
    br.SkipBits(1); // no_output_of_prior_pics_flag
    br.SkipBits(1); // long_term_reference_flag
  } else if (out.slice_type != 2) {
    // adaptive_ref_pic_marking_mode_flag for non-IDR ref NALs — present when nal_ref_idc != 0
    // Caller only feeds ref slices from typical WebRTC CBP; skip if flag set with MMCO.
    // We detect nal_ref via idr/non-idr type; non-IDR slices from encoder are refs.
    // For nal_ref_idc: read from original NAL header byte.
  }
  // Re-parse marking using nal_ref_idc from first byte.
  // Simpler path: for CBP I/P with no MMCO, adaptive flag is 0.
  // The bits above for idr already consumed dec_ref_pic_marking.
  // For non-IDR P with nal_ref_idc!=0 we still need adaptive_ref_pic_marking_mode_flag.
  if (!idr) {
    const int nal_ref_idc = (rbsp[0] >> 5) & 0x3;
    if (nal_ref_idc != 0) {
      if (br.ReadBits(1)) {
        for (;;) {
          const uint32_t op = br.ReadUe();
          if (op == 0) {
            break;
          }
          if (op == 1 || op == 3) {
            (void)br.ReadUe();
          }
          if (op == 2) {
            (void)br.ReadUe();
          }
          if (op == 3 || op == 6) {
            (void)br.ReadUe();
          }
          if (op == 4) {
            (void)br.ReadUe();
          }
          if (op == 5) {
            (void)br.ReadUe();
          }
        }
      }
    }
  }
  if (pps.entropy_coding_mode_flag && out.slice_type != 2) {
    out.cabac_init_idc = static_cast<int>(br.ReadUe());
  }
  out.slice_qp_delta = br.ReadSe();
  if (pps.deblocking_filter_control_present_flag) {
    out.disable_deblocking_filter_idc = static_cast<int>(br.ReadUe());
    if (out.disable_deblocking_filter_idc != 1) {
      out.slice_alpha_c0_offset_div2 = br.ReadSe();
      out.slice_beta_offset_div2 = br.ReadSe();
    }
  }
  if (!br.Ok()) {
    return false;
  }
  out.header_bit_size = br.BitsRead();
  return true;
}

// ---- NV12 helpers ----------------------------------------------------

void FillNv12(uint8_t* y_dst, size_t y_stride, uint8_t* uv_dst, size_t uv_stride,
              const VideoFrameI420& frame) {
  const int width = frame.width;
  const int height = frame.height;
  for (int row = 0; row < height; ++row) {
    std::memcpy(y_dst + static_cast<size_t>(row) * y_stride,
                frame.y.data() + static_cast<size_t>(row) * width, static_cast<size_t>(width));
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
}

bool Nv12ToRgba(const uint8_t* y_src, size_t y_stride, const uint8_t* uv_src, size_t uv_stride,
                int width, int height, VideoFrameRgba& out) {
  VideoFrameI420 i420;
  i420.width = width;
  i420.height = height;
  i420.y.resize(static_cast<size_t>(width * height));
  i420.u.resize(static_cast<size_t>((width / 2) * (height / 2)));
  i420.v.resize(i420.u.size());
  for (int row = 0; row < height; ++row) {
    std::memcpy(i420.y.data() + static_cast<size_t>(row) * width, y_src + static_cast<size_t>(row) * y_stride,
                static_cast<size_t>(width));
  }
  const int chroma_width = width / 2;
  const int chroma_height = height / 2;
  for (int row = 0; row < chroma_height; ++row) {
    const uint8_t* uv_row = uv_src + static_cast<size_t>(row) * uv_stride;
    for (int col = 0; col < chroma_width; ++col) {
      i420.u[static_cast<size_t>(row * chroma_width + col)] = uv_row[col * 2 + 0];
      i420.v[static_cast<size_t>(row * chroma_width + col)] = uv_row[col * 2 + 1];
    }
  }
  return I420ToRgba(i420, out);
}

// ---- Display / capability probe --------------------------------------

struct VaCaps {
  bool has_encode = false;
  bool has_decode = false;
  VAProfile encode_profile = VAProfileH264ConstrainedBaseline;
  VAEntrypoint encode_entrypoint = VAEntrypointEncSlice;
  VAProfile decode_profile = VAProfileH264ConstrainedBaseline;
  bool decode_cbp = false;
  bool decode_main = false;
  bool decode_high = false;
  uint32_t rc_mode = VA_RC_CBR;
  uint32_t packed_headers = 0;
};

// VideoToolbox (and some MFTs) may emit Main/High even when CBP was requested.
// Pick the tightest VA profile that can accept the bitstream.
VAProfile SelectDecodeProfile(const VaCaps& caps, const ParsedSps& sps, const ParsedPps& pps) {
  // Honor SPS profile_idc. Do not upgrade Baseline solely because PPS more_rbsp_data was
  // misread (VideoToolbox CBP PPS ends with rbsp_trailing 0x80).
  const bool needs_high = sps.profile_idc == 100 || sps.profile_idc == 110 ||
                          sps.profile_idc == 122 || sps.profile_idc == 244 ||
                          (pps.transform_8x8_mode_flag != 0 && sps.profile_idc >= 100);
  const bool needs_main =
      needs_high || sps.profile_idc == 77 || pps.entropy_coding_mode_flag != 0;
  if (needs_high && caps.decode_high) {
    return VAProfileH264High;
  }
  if (needs_main && caps.decode_main) {
    return VAProfileH264Main;
  }
  if (needs_high && caps.decode_main) {
    // High stream on a Main-only host — still try Main (common Intel path).
    return VAProfileH264Main;
  }
  if (caps.decode_cbp) {
    return VAProfileH264ConstrainedBaseline;
  }
  if (caps.decode_main) {
    return VAProfileH264Main;
  }
  if (caps.decode_high) {
    return VAProfileH264High;
  }
  return caps.decode_profile;
}

struct VaDisplayState {
  int drm_fd = -1;
  VADisplay display = nullptr;
};

void CloseVaDisplay(VaDisplayState& st) {
  if (st.display) {
    vaTerminate(st.display);
    st.display = nullptr;
  }
  if (st.drm_fd >= 0) {
    close(st.drm_fd);
    st.drm_fd = -1;
  }
}

bool QueryProfileEntrypoint(VADisplay dpy, VAProfile profile, VAEntrypoint entry) {
  const int max_ep = vaMaxNumEntrypoints(dpy);
  if (max_ep <= 0) {
    return false;
  }
  std::vector<VAEntrypoint> eps(static_cast<size_t>(max_ep));
  int num = 0;
  if (vaQueryConfigEntrypoints(dpy, profile, eps.data(), &num) != VA_STATUS_SUCCESS) {
    return false;
  }
  for (int i = 0; i < num; ++i) {
    if (eps[static_cast<size_t>(i)] == entry) {
      return true;
    }
  }
  return false;
}

bool ProbeCaps(VADisplay dpy, VaCaps& caps) {
  caps = {};
  static const VAProfile kEncProfiles[] = {VAProfileH264ConstrainedBaseline, VAProfileH264Main,
                                           VAProfileH264High};
  static const VAEntrypoint kEncEntries[] = {VAEntrypointEncSlice, VAEntrypointEncSliceLP};
  for (VAProfile profile : kEncProfiles) {
    for (VAEntrypoint entry : kEncEntries) {
      if (!QueryProfileEntrypoint(dpy, profile, entry)) {
        continue;
      }
      VAConfigAttrib attrib{};
      attrib.type = VAConfigAttribRTFormat;
      if (vaGetConfigAttributes(dpy, profile, entry, &attrib, 1) != VA_STATUS_SUCCESS) {
        continue;
      }
      if ((attrib.value & VA_RT_FORMAT_YUV420) == 0) {
        continue;
      }
      caps.has_encode = true;
      caps.encode_profile = profile;
      caps.encode_entrypoint = entry;

      VAConfigAttrib attrs[3]{};
      attrs[0].type = VAConfigAttribRateControl;
      attrs[1].type = VAConfigAttribEncPackedHeaders;
      attrs[2].type = VAConfigAttribRTFormat;
      if (vaGetConfigAttributes(dpy, profile, entry, attrs, 3) == VA_STATUS_SUCCESS) {
        const uint32_t rc = attrs[0].value;
        if (rc & VA_RC_CBR) {
          caps.rc_mode = VA_RC_CBR;
        } else if (rc & VA_RC_VBR) {
          caps.rc_mode = VA_RC_VBR;
        } else if (rc & VA_RC_CQP) {
          caps.rc_mode = VA_RC_CQP;
        } else {
          caps.rc_mode = VA_RC_NONE;
        }
        if (attrs[1].value != VA_ATTRIB_NOT_SUPPORTED) {
          caps.packed_headers = attrs[1].value &
                                (VA_ENC_PACKED_HEADER_SEQUENCE | VA_ENC_PACKED_HEADER_PICTURE);
        }
      }
      break;
    }
    if (caps.has_encode) {
      break;
    }
  }

  caps.decode_cbp = QueryProfileEntrypoint(dpy, VAProfileH264ConstrainedBaseline, VAEntrypointVLD);
  caps.decode_main = QueryProfileEntrypoint(dpy, VAProfileH264Main, VAEntrypointVLD);
  caps.decode_high = QueryProfileEntrypoint(dpy, VAProfileH264High, VAEntrypointVLD);
  caps.has_decode = caps.decode_cbp || caps.decode_main || caps.decode_high;
  // Default probe pick; EnsureDecoderContext overrides from SPS/PPS (Mac VT often emits Main).
  if (caps.decode_cbp) {
    caps.decode_profile = VAProfileH264ConstrainedBaseline;
  } else if (caps.decode_main) {
    caps.decode_profile = VAProfileH264Main;
  } else if (caps.decode_high) {
    caps.decode_profile = VAProfileH264High;
  }
  return caps.has_encode || caps.has_decode;
}

bool OpenFirstUsableDisplay(VaDisplayState& st, VaCaps& caps, std::string& reason) {
  CloseVaDisplay(st);
  DIR* dir = opendir("/dev/dri");
  if (!dir) {
    reason = "no /dev/dri (DRM render nodes unavailable)";
    return false;
  }
  std::vector<std::string> nodes;
  while (dirent* ent = readdir(dir)) {
    if (std::strncmp(ent->d_name, "renderD", 7) == 0) {
      nodes.push_back(std::string("/dev/dri/") + ent->d_name);
    }
  }
  closedir(dir);
  std::sort(nodes.begin(), nodes.end());

  for (const std::string& path : nodes) {
    const int fd = open(path.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0) {
      continue;
    }
    VADisplay dpy = vaGetDisplayDRM(fd);
    if (!dpy) {
      close(fd);
      continue;
    }
    int major = 0;
    int minor = 0;
    if (vaInitialize(dpy, &major, &minor) != VA_STATUS_SUCCESS) {
      close(fd);
      continue;
    }
    VaCaps local{};
    if (!ProbeCaps(dpy, local)) {
      vaTerminate(dpy);
      close(fd);
      continue;
    }
    st.drm_fd = fd;
    st.display = dpy;
    caps = local;
    return true;
  }
  reason = "no DRM render node with H264 EncSlice/VLD";
  return false;
}

std::vector<uint8_t> BuildSpsNal(int width, int height, int level_idc, int constraint_set_flags) {
  const int width_mbs = Align16(width) / 16;
  const int height_mbs = Align16(height) / 16;
  const int crop_right = (Align16(width) - width) / 2;
  const int crop_bottom = (Align16(height) - height) / 2;

  BitWriter bw;
  bw.PutBits(0, 1);
  bw.PutBits(3, 2); // nal_ref_idc
  bw.PutBits(7, 5); // SPS
  bw.PutBits(66, 8); // Baseline profile_idc
  bw.PutBits(!!(constraint_set_flags & 1), 1);
  bw.PutBits(!!(constraint_set_flags & 2), 1);
  bw.PutBits(!!(constraint_set_flags & 4), 1);
  bw.PutBits(!!(constraint_set_flags & 8), 1);
  bw.PutBits(0, 4);
  bw.PutBits(static_cast<uint32_t>(level_idc), 8);
  bw.PutUe(0); // sps id
  bw.PutUe(0); // log2_max_frame_num_minus4
  bw.PutUe(0); // pic_order_cnt_type
  bw.PutUe(0); // log2_max_pic_order_cnt_lsb_minus4
  bw.PutUe(1); // num_ref_frames
  bw.PutBits(0, 1);
  bw.PutUe(static_cast<uint32_t>(width_mbs - 1));
  bw.PutUe(static_cast<uint32_t>(height_mbs - 1));
  bw.PutBits(1, 1); // frame_mbs_only
  bw.PutBits(1, 1); // direct_8x8_inference
  if (crop_right || crop_bottom) {
    bw.PutBits(1, 1);
    bw.PutUe(0);
    bw.PutUe(static_cast<uint32_t>(crop_right));
    bw.PutUe(0);
    bw.PutUe(static_cast<uint32_t>(crop_bottom));
  } else {
    bw.PutBits(0, 1);
  }
  bw.PutBits(0, 1); // vui
  bw.RbspTrailingBits();
  return bw.Bytes();
}

std::vector<uint8_t> BuildPpsNal() {
  BitWriter bw;
  bw.PutBits(0, 1);
  bw.PutBits(3, 2);
  bw.PutBits(8, 5); // PPS
  bw.PutUe(0);
  bw.PutUe(0);
  bw.PutBits(0, 1); // cavlc
  bw.PutBits(0, 1); // pic_order_present
  bw.PutUe(0);      // slice groups
  bw.PutUe(0);      // ref l0
  bw.PutUe(0);      // ref l1
  bw.PutBits(0, 1);
  bw.PutBits(0, 2);
  bw.PutSe(0); // pic_init_qp_minus26 (qp=26)
  bw.PutSe(0);
  bw.PutSe(0);
  bw.PutBits(1, 1); // deblocking control present
  bw.PutBits(0, 1);
  bw.PutBits(0, 1);
  bw.RbspTrailingBits();
  return bw.Bytes();
}

bool FillMappedImage(VAImage& image, void* ptr, const VideoFrameI420& frame) {
  auto* base = static_cast<uint8_t*>(ptr);
  if (image.format.fourcc == VA_FOURCC_NV12 && image.num_planes >= 2) {
    FillNv12(base + image.offsets[0], image.pitches[0], base + image.offsets[1], image.pitches[1],
             frame);
    return true;
  }
  if (image.format.fourcc == VA_FOURCC_I420 || image.format.fourcc == VA_FOURCC_YV12) {
    for (int row = 0; row < frame.height; ++row) {
      std::memcpy(base + image.offsets[0] + static_cast<size_t>(row) * image.pitches[0],
                  frame.y.data() + static_cast<size_t>(row) * frame.width,
                  static_cast<size_t>(frame.width));
    }
    const int cw = frame.width / 2;
    const int ch = frame.height / 2;
    const int u_plane = (image.format.fourcc == VA_FOURCC_YV12) ? 2 : 1;
    const int v_plane = (image.format.fourcc == VA_FOURCC_YV12) ? 1 : 2;
    for (int row = 0; row < ch; ++row) {
      std::memcpy(base + image.offsets[u_plane] + static_cast<size_t>(row) * image.pitches[u_plane],
                  frame.u.data() + static_cast<size_t>(row) * cw, static_cast<size_t>(cw));
      std::memcpy(base + image.offsets[v_plane] + static_cast<size_t>(row) * image.pitches[v_plane],
                  frame.v.data() + static_cast<size_t>(row) * cw, static_cast<size_t>(cw));
    }
    return true;
  }
  return false;
}

bool UploadI420ToSurface(VADisplay dpy, VASurfaceID surface, const VideoFrameI420& frame) {
  // Prefer derive (zero-copy). Some drivers (e.g. radeonsi encode surfaces) reject it —
  // fall back to vaCreateImage + vaPutImage with NV12.
  VAImage image{};
  if (vaDeriveImage(dpy, surface, &image) == VA_STATUS_SUCCESS) {
    void* ptr = nullptr;
    if (vaMapBuffer(dpy, image.buf, &ptr) == VA_STATUS_SUCCESS && ptr &&
        FillMappedImage(image, ptr, frame)) {
      vaUnmapBuffer(dpy, image.buf);
      vaDestroyImage(dpy, image.image_id);
      return true;
    }
    if (ptr) {
      vaUnmapBuffer(dpy, image.buf);
    }
    vaDestroyImage(dpy, image.image_id);
  }

  VAImageFormat fmt{};
  fmt.fourcc = VA_FOURCC_NV12;
  fmt.byte_order = VA_LSB_FIRST;
  fmt.bits_per_pixel = 12;
  if (vaCreateImage(dpy, &fmt, frame.width, frame.height, &image) != VA_STATUS_SUCCESS) {
    fmt.fourcc = VA_FOURCC_I420;
    if (vaCreateImage(dpy, &fmt, frame.width, frame.height, &image) != VA_STATUS_SUCCESS) {
      return false;
    }
  }
  void* ptr = nullptr;
  if (vaMapBuffer(dpy, image.buf, &ptr) != VA_STATUS_SUCCESS || !ptr ||
      !FillMappedImage(image, ptr, frame)) {
    if (ptr) {
      vaUnmapBuffer(dpy, image.buf);
    }
    vaDestroyImage(dpy, image.image_id);
    return false;
  }
  vaUnmapBuffer(dpy, image.buf);
  const VAStatus put = vaPutImage(dpy, surface, image.image_id, 0, 0, frame.width, frame.height, 0, 0,
                                  frame.width, frame.height);
  vaDestroyImage(dpy, image.image_id);
  return put == VA_STATUS_SUCCESS;
}

bool DownloadSurfaceToRgba(VADisplay dpy, VASurfaceID surface, int width, int height,
                           VideoFrameRgba& out) {
  if (vaSyncSurface(dpy, surface) != VA_STATUS_SUCCESS) {
    return false;
  }

  auto image_to_rgba = [&](VAImage& image, void* ptr) -> bool {
    auto* base = static_cast<uint8_t*>(ptr);
    if (image.format.fourcc == VA_FOURCC_NV12 && image.num_planes >= 2) {
      return Nv12ToRgba(base + image.offsets[0], image.pitches[0], base + image.offsets[1],
                        image.pitches[1], width, height, out);
    }
    if (image.format.fourcc == VA_FOURCC_I420 || image.format.fourcc == VA_FOURCC_YV12) {
      VideoFrameI420 i420;
      i420.width = width;
      i420.height = height;
      i420.y.resize(static_cast<size_t>(width * height));
      i420.u.resize(static_cast<size_t>((width / 2) * (height / 2)));
      i420.v.resize(i420.u.size());
      for (int row = 0; row < height; ++row) {
        std::memcpy(i420.y.data() + static_cast<size_t>(row) * width,
                    base + image.offsets[0] + static_cast<size_t>(row) * image.pitches[0],
                    static_cast<size_t>(width));
      }
      const int cw = width / 2;
      const int ch = height / 2;
      const int u_plane = (image.format.fourcc == VA_FOURCC_YV12) ? 2 : 1;
      const int v_plane = (image.format.fourcc == VA_FOURCC_YV12) ? 1 : 2;
      for (int row = 0; row < ch; ++row) {
        std::memcpy(i420.u.data() + static_cast<size_t>(row) * cw,
                    base + image.offsets[u_plane] + static_cast<size_t>(row) * image.pitches[u_plane],
                    static_cast<size_t>(cw));
        std::memcpy(i420.v.data() + static_cast<size_t>(row) * cw,
                    base + image.offsets[v_plane] + static_cast<size_t>(row) * image.pitches[v_plane],
                    static_cast<size_t>(cw));
      }
      return I420ToRgba(i420, out);
    }
    return false;
  };

  VAImage image{};
  if (vaDeriveImage(dpy, surface, &image) == VA_STATUS_SUCCESS) {
    void* ptr = nullptr;
    if (vaMapBuffer(dpy, image.buf, &ptr) == VA_STATUS_SUCCESS && ptr && image_to_rgba(image, ptr)) {
      vaUnmapBuffer(dpy, image.buf);
      vaDestroyImage(dpy, image.image_id);
      return true;
    }
    if (ptr) {
      vaUnmapBuffer(dpy, image.buf);
    }
    vaDestroyImage(dpy, image.image_id);
  }

  VAImageFormat fmt{};
  fmt.fourcc = VA_FOURCC_NV12;
  fmt.byte_order = VA_LSB_FIRST;
  fmt.bits_per_pixel = 12;
  if (vaCreateImage(dpy, &fmt, width, height, &image) != VA_STATUS_SUCCESS) {
    fmt.fourcc = VA_FOURCC_I420;
    if (vaCreateImage(dpy, &fmt, width, height, &image) != VA_STATUS_SUCCESS) {
      return false;
    }
  }
  if (vaGetImage(dpy, surface, 0, 0, width, height, image.image_id) != VA_STATUS_SUCCESS) {
    vaDestroyImage(dpy, image.image_id);
    return false;
  }
  void* ptr = nullptr;
  if (vaMapBuffer(dpy, image.buf, &ptr) != VA_STATUS_SUCCESS || !ptr || !image_to_rgba(image, ptr)) {
    if (ptr) {
      vaUnmapBuffer(dpy, image.buf);
    }
    vaDestroyImage(dpy, image.image_id);
    return false;
  }
  vaUnmapBuffer(dpy, image.buf);
  vaDestroyImage(dpy, image.image_id);
  return true;
}

class VaapiVideoCodec final : public IVideoCodec {
public:
  VaapiVideoCodec(VaDisplayState display, VaCaps caps)
      : display_(std::move(display)), caps_(caps) {}
  ~VaapiVideoCodec() override {
    ResetEncoder();
    ResetDecoder();
    CloseVaDisplay(display_);
  }

  std::string BackendName() const override { return "vaapi"; }
  bool HasEncoder() const override { return enc_ready_; }
  bool HasDecoder() const override { return decoder_configured_; }

  Roe<void> ConfigureEncoder(int width, int height, int fps) override;
  Roe<void> ConfigureDecoder() override;
  Roe<EncodedAccessUnit> Encode(const VideoFrameI420& frame, bool force_keyframe) override;
  Roe<VideoFrameRgba> Decode(const uint8_t* annex_b, size_t size) override;
  void ResetEncoder() override;
  void ResetDecoder() override;

  bool CapsHasEncoder() const { return caps_.has_encode; }
  bool CapsHasDecoder() const { return caps_.has_decode; }

private:
  bool EnsureDecoderContext(const ParsedSps& sps, std::string& error);
  Roe<VideoFrameRgba> DecodeSliceAccessUnit(const std::vector<AnnexBNal>& nals);

  VaDisplayState display_;
  VaCaps caps_;

  // Encode
  bool enc_ready_ = false;
  VAConfigID enc_config_ = VA_INVALID_ID;
  VAContextID enc_context_ = VA_INVALID_ID;
  VASurfaceID enc_input_ = VA_INVALID_ID;
  VASurfaceID enc_recon_[2] = {VA_INVALID_ID, VA_INVALID_ID};
  VABufferID enc_coded_ = VA_INVALID_ID;
  int enc_width_ = 0;
  int enc_height_ = 0;
  int enc_aligned_w_ = 0;
  int enc_aligned_h_ = 0;
  int enc_fps_ = 0;
  int enc_bitrate_ = 0;
  int enc_frame_num_ = 0;
  int enc_idr_pic_id_ = 0;
  int enc_poc_ = 0;
  int enc_frames_since_idr_ = 0;
  bool enc_have_ref_ = false;
  int enc_ref_slot_ = 0;
  std::vector<uint8_t> sps_nal_;
  std::vector<uint8_t> pps_nal_;
  int constraint_set_flags_ = 0x3; // set0|set1 for CBP

  // Decode
  bool decoder_configured_ = false;
  VAConfigID dec_config_ = VA_INVALID_ID;
  VAContextID dec_context_ = VA_INVALID_ID;
  std::array<VASurfaceID, kMaxDpbSurfaces> dec_surfaces_{};
  int dec_next_surface_ = 0;
  ParsedSps dec_sps_{};
  ParsedPps dec_pps_{};
  bool dec_have_sps_ = false;
  bool dec_have_pps_ = false;
  VAPictureH264 dec_ref_{};
  bool dec_have_ref_ = false;
  int dec_width_ = 0;
  int dec_height_ = 0;
  VAProfile dec_active_profile_ = VAProfileH264ConstrainedBaseline;
};

Roe<void> VaapiVideoCodec::ConfigureEncoder(int width, int height, int fps) {
  ResetEncoder();
  if (!caps_.has_encode) {
    return Error("VA-API H264 encoder unavailable on this device");
  }
  if (width <= 0 || height <= 0 || fps <= 0) {
    return Error("invalid H264 encoder configuration");
  }
  if ((width & 1) || (height & 1)) {
    return Error("H264 encoder requires even dimensions");
  }

  enc_width_ = width;
  enc_height_ = height;
  enc_aligned_w_ = Align16(width);
  enc_aligned_h_ = Align16(height);
  enc_fps_ = fps;
  enc_bitrate_ = EstimateBitrateBps(width, height, fps);

  VAConfigAttrib attrs[3]{};
  int attr_n = 0;
  attrs[attr_n].type = VAConfigAttribRTFormat;
  attrs[attr_n].value = VA_RT_FORMAT_YUV420;
  ++attr_n;
  attrs[attr_n].type = VAConfigAttribRateControl;
  attrs[attr_n].value = caps_.rc_mode;
  ++attr_n;
  if (caps_.packed_headers) {
    attrs[attr_n].type = VAConfigAttribEncPackedHeaders;
    attrs[attr_n].value = caps_.packed_headers;
    ++attr_n;
  }

  if (vaCreateConfig(display_.display, caps_.encode_profile, caps_.encode_entrypoint, attrs, attr_n,
                     &enc_config_) != VA_STATUS_SUCCESS) {
    return Error("vaCreateConfig (encode) failed");
  }

  VASurfaceID surfaces[3];
  if (vaCreateSurfaces(display_.display, VA_RT_FORMAT_YUV420,
                       static_cast<unsigned>(enc_aligned_w_), static_cast<unsigned>(enc_aligned_h_),
                       surfaces, 3, nullptr, 0) != VA_STATUS_SUCCESS) {
    ResetEncoder();
    return Error("vaCreateSurfaces (encode) failed");
  }
  enc_input_ = surfaces[0];
  enc_recon_[0] = surfaces[1];
  enc_recon_[1] = surfaces[2];

  if (vaCreateContext(display_.display, enc_config_, enc_aligned_w_, enc_aligned_h_, VA_PROGRESSIVE,
                      surfaces, 3, &enc_context_) != VA_STATUS_SUCCESS) {
    ResetEncoder();
    return Error("vaCreateContext (encode) failed");
  }

  const unsigned coded_size =
      static_cast<unsigned>((static_cast<size_t>(enc_aligned_w_) * enc_aligned_h_ * 400) / 256);
  if (vaCreateBuffer(display_.display, enc_context_, VAEncCodedBufferType, coded_size, 1, nullptr,
                     &enc_coded_) != VA_STATUS_SUCCESS) {
    ResetEncoder();
    return Error("vaCreateBuffer (coded) failed");
  }

  if (caps_.encode_profile == VAProfileH264ConstrainedBaseline) {
    constraint_set_flags_ = 0x3;
  } else if (caps_.encode_profile == VAProfileH264Main) {
    constraint_set_flags_ = 0x2;
  } else {
    constraint_set_flags_ = 0x8;
  }
  sps_nal_ = BuildSpsNal(width, height, 31, constraint_set_flags_);
  pps_nal_ = BuildPpsNal();

  enc_frame_num_ = 0;
  enc_idr_pic_id_ = 0;
  enc_poc_ = 0;
  enc_frames_since_idr_ = 0;
  enc_have_ref_ = false;
  enc_ref_slot_ = 0;
  enc_ready_ = true;
  return {};
}

Roe<EncodedAccessUnit> VaapiVideoCodec::Encode(const VideoFrameI420& frame, bool force_keyframe) {
  if (!enc_ready_) {
    return Error("H264 encoder not configured");
  }
  if (frame.width != enc_width_ || frame.height != enc_height_) {
    return Error("encoder frame size does not match ConfigureEncoder size");
  }
  if (!UploadI420ToSurface(display_.display, enc_input_, frame)) {
    return Error("failed to upload frame to VA surface");
  }

  const bool idr = force_keyframe || !enc_have_ref_ || enc_frames_since_idr_ >= kIntraIdrPeriod;
  const int recon_slot = enc_have_ref_ ? (1 - enc_ref_slot_) : 0;

  VAEncSequenceParameterBufferH264 seq{};
  seq.level_idc = 31;
  seq.intra_period = kIntraIdrPeriod;
  seq.intra_idr_period = kIntraIdrPeriod;
  seq.ip_period = 1;
  seq.bits_per_second = static_cast<uint32_t>(enc_bitrate_);
  seq.max_num_ref_frames = 1;
  seq.picture_width_in_mbs = static_cast<uint16_t>(enc_aligned_w_ / 16);
  seq.picture_height_in_mbs = static_cast<uint16_t>(enc_aligned_h_ / 16);
  seq.seq_fields.bits.chroma_format_idc = 1;
  seq.seq_fields.bits.frame_mbs_only_flag = 1;
  seq.seq_fields.bits.direct_8x8_inference_flag = 1;
  seq.seq_fields.bits.log2_max_frame_num_minus4 = 0;
  seq.seq_fields.bits.pic_order_cnt_type = 0;
  seq.seq_fields.bits.log2_max_pic_order_cnt_lsb_minus4 = 0;
  if (enc_aligned_w_ != enc_width_ || enc_aligned_h_ != enc_height_) {
    seq.frame_cropping_flag = 1;
    seq.frame_crop_right_offset = static_cast<uint32_t>((enc_aligned_w_ - enc_width_) / 2);
    seq.frame_crop_bottom_offset = static_cast<uint32_t>((enc_aligned_h_ - enc_height_) / 2);
  }

  VAEncPictureParameterBufferH264 pic{};
  InvalidatePicture(pic.CurrPic);
  for (auto& ref : pic.ReferenceFrames) {
    InvalidatePicture(ref);
  }
  pic.CurrPic.picture_id = enc_recon_[recon_slot];
  pic.CurrPic.frame_idx = enc_frame_num_;
  pic.CurrPic.flags = VA_PICTURE_H264_SHORT_TERM_REFERENCE;
  pic.CurrPic.TopFieldOrderCnt = enc_poc_;
  pic.CurrPic.BottomFieldOrderCnt = enc_poc_;
  if (!idr && enc_have_ref_) {
    pic.ReferenceFrames[0].picture_id = enc_recon_[enc_ref_slot_];
    pic.ReferenceFrames[0].frame_idx = std::max(0, enc_frame_num_ - 1);
    pic.ReferenceFrames[0].flags = VA_PICTURE_H264_SHORT_TERM_REFERENCE;
    pic.ReferenceFrames[0].TopFieldOrderCnt = std::max(0, enc_poc_ - 2);
    pic.ReferenceFrames[0].BottomFieldOrderCnt = pic.ReferenceFrames[0].TopFieldOrderCnt;
  }
  pic.coded_buf = enc_coded_;
  pic.frame_num = static_cast<uint16_t>(enc_frame_num_);
  pic.pic_init_qp = 26;
  pic.num_ref_idx_l0_active_minus1 = 0;
  pic.num_ref_idx_l1_active_minus1 = 0;
  pic.pic_fields.bits.idr_pic_flag = idr ? 1 : 0;
  pic.pic_fields.bits.reference_pic_flag = 1;
  pic.pic_fields.bits.entropy_coding_mode_flag = 0;
  pic.pic_fields.bits.transform_8x8_mode_flag = 0;
  pic.pic_fields.bits.deblocking_filter_control_present_flag = 1;

  VAEncSliceParameterBufferH264 slice{};
  slice.macroblock_address = 0;
  slice.num_macroblocks = static_cast<uint32_t>((enc_aligned_w_ / 16) * (enc_aligned_h_ / 16));
  slice.macroblock_info = VA_INVALID_ID;
  slice.slice_type = idr ? 2 : 0; // I : P
  slice.pic_parameter_set_id = 0;
  slice.idr_pic_id = static_cast<uint16_t>(enc_idr_pic_id_);
  slice.pic_order_cnt_lsb = static_cast<uint16_t>(enc_poc_ & 0xFFFF);
  slice.num_ref_idx_active_override_flag = 0;
  for (auto& r : slice.RefPicList0) {
    InvalidatePicture(r);
  }
  for (auto& r : slice.RefPicList1) {
    InvalidatePicture(r);
  }
  if (!idr && enc_have_ref_) {
    slice.RefPicList0[0] = pic.ReferenceFrames[0];
  }
  slice.slice_qp_delta = 0;
  slice.disable_deblocking_filter_idc = 0;

  std::vector<VABufferID> buffers;
  buffers.reserve(16);

  auto destroy_temps = [&]() {
    for (VABufferID id : buffers) {
      if (id != VA_INVALID_ID) {
        vaDestroyBuffer(display_.display, id);
      }
    }
    buffers.clear();
  };

  VABufferID seq_buf = VA_INVALID_ID;
  if (vaCreateBuffer(display_.display, enc_context_, VAEncSequenceParameterBufferType, sizeof(seq), 1,
                     &seq, &seq_buf) != VA_STATUS_SUCCESS) {
    return Error("vaCreateBuffer (seq) failed");
  }
  buffers.push_back(seq_buf);

  if (caps_.rc_mode == VA_RC_CBR || caps_.rc_mode == VA_RC_VBR) {
    VABufferID rc_buf = VA_INVALID_ID;
    if (vaCreateBuffer(display_.display, enc_context_, VAEncMiscParameterBufferType,
                       sizeof(VAEncMiscParameterBuffer) + sizeof(VAEncMiscParameterRateControl), 1,
                       nullptr, &rc_buf) == VA_STATUS_SUCCESS) {
      VAEncMiscParameterBuffer* misc = nullptr;
      if (vaMapBuffer(display_.display, rc_buf, reinterpret_cast<void**>(&misc)) == VA_STATUS_SUCCESS &&
          misc) {
        misc->type = VAEncMiscParameterTypeRateControl;
        auto* rc = reinterpret_cast<VAEncMiscParameterRateControl*>(misc->data);
        std::memset(rc, 0, sizeof(*rc));
        rc->bits_per_second = static_cast<uint32_t>(enc_bitrate_);
        rc->target_percentage = 80;
        rc->window_size = 500;
        rc->initial_qp = 26;
        rc->min_qp = 18;
        rc->max_qp = 40;
        vaUnmapBuffer(display_.display, rc_buf);
        buffers.push_back(rc_buf);
      } else {
        vaDestroyBuffer(display_.display, rc_buf);
      }
    }
  }

  VABufferID pic_buf = VA_INVALID_ID;
  if (vaCreateBuffer(display_.display, enc_context_, VAEncPictureParameterBufferType, sizeof(pic), 1,
                     &pic, &pic_buf) != VA_STATUS_SUCCESS) {
    destroy_temps();
    return Error("vaCreateBuffer (pic) failed");
  }
  buffers.push_back(pic_buf);

  if (idr && (caps_.packed_headers & VA_ENC_PACKED_HEADER_SEQUENCE)) {
    VAEncPackedHeaderParameterBuffer ph{};
    ph.type = VAEncPackedHeaderSequence;
    ph.bit_length = static_cast<uint32_t>(sps_nal_.size() * 8);
    ph.has_emulation_bytes = 0;
    VABufferID ph_param = VA_INVALID_ID;
    VABufferID ph_data = VA_INVALID_ID;
    if (vaCreateBuffer(display_.display, enc_context_, VAEncPackedHeaderParameterBufferType,
                       sizeof(ph), 1, &ph, &ph_param) == VA_STATUS_SUCCESS &&
        vaCreateBuffer(display_.display, enc_context_, VAEncPackedHeaderDataBufferType,
                       static_cast<unsigned>(sps_nal_.size()), 1, sps_nal_.data(), &ph_data) ==
            VA_STATUS_SUCCESS) {
      buffers.push_back(ph_param);
      buffers.push_back(ph_data);
    } else {
      if (ph_param != VA_INVALID_ID) {
        vaDestroyBuffer(display_.display, ph_param);
      }
      if (ph_data != VA_INVALID_ID) {
        vaDestroyBuffer(display_.display, ph_data);
      }
    }
  }
  if (idr && (caps_.packed_headers & VA_ENC_PACKED_HEADER_PICTURE)) {
    VAEncPackedHeaderParameterBuffer ph{};
    ph.type = VAEncPackedHeaderPicture;
    ph.bit_length = static_cast<uint32_t>(pps_nal_.size() * 8);
    ph.has_emulation_bytes = 0;
    VABufferID ph_param = VA_INVALID_ID;
    VABufferID ph_data = VA_INVALID_ID;
    if (vaCreateBuffer(display_.display, enc_context_, VAEncPackedHeaderParameterBufferType,
                       sizeof(ph), 1, &ph, &ph_param) == VA_STATUS_SUCCESS &&
        vaCreateBuffer(display_.display, enc_context_, VAEncPackedHeaderDataBufferType,
                       static_cast<unsigned>(pps_nal_.size()), 1, pps_nal_.data(), &ph_data) ==
            VA_STATUS_SUCCESS) {
      buffers.push_back(ph_param);
      buffers.push_back(ph_data);
    } else {
      if (ph_param != VA_INVALID_ID) {
        vaDestroyBuffer(display_.display, ph_param);
      }
      if (ph_data != VA_INVALID_ID) {
        vaDestroyBuffer(display_.display, ph_data);
      }
    }
  }

  VABufferID slice_buf = VA_INVALID_ID;
  if (vaCreateBuffer(display_.display, enc_context_, VAEncSliceParameterBufferType, sizeof(slice), 1,
                     &slice, &slice_buf) != VA_STATUS_SUCCESS) {
    destroy_temps();
    return Error("vaCreateBuffer (slice) failed");
  }
  buffers.push_back(slice_buf);

  if (vaBeginPicture(display_.display, enc_context_, enc_input_) != VA_STATUS_SUCCESS) {
    destroy_temps();
    return Error("vaBeginPicture (encode) failed");
  }
  if (vaRenderPicture(display_.display, enc_context_, buffers.data(),
                      static_cast<int>(buffers.size())) != VA_STATUS_SUCCESS) {
    vaEndPicture(display_.display, enc_context_);
    destroy_temps();
    return Error("vaRenderPicture (encode) failed");
  }
  if (vaEndPicture(display_.display, enc_context_) != VA_STATUS_SUCCESS) {
    destroy_temps();
    return Error("vaEndPicture (encode) failed");
  }
  destroy_temps();

  if (vaSyncSurface(display_.display, enc_input_) != VA_STATUS_SUCCESS) {
    return Error("vaSyncSurface (encode) failed");
  }

  VACodedBufferSegment* segment = nullptr;
  if (vaMapBuffer(display_.display, enc_coded_, reinterpret_cast<void**>(&segment)) !=
          VA_STATUS_SUCCESS ||
      !segment) {
    return Error("vaMapBuffer (coded) failed");
  }

  EncodedAccessUnit unit;
  unit.keyframe = idr;
  if (idr) {
    AppendStartCodeAndNal(unit.annex_b, sps_nal_.data(), sps_nal_.size());
    AppendStartCodeAndNal(unit.annex_b, pps_nal_.data(), pps_nal_.size());
  }
  for (VACodedBufferSegment* s = segment; s; s = reinterpret_cast<VACodedBufferSegment*>(s->next)) {
    if (!s->buf || s->size == 0) {
      continue;
    }
    const auto* bytes = static_cast<const uint8_t*>(s->buf);
    // Drivers usually emit start-code prefixed NALs; accept either form.
    if (s->size >= 4 && bytes[0] == 0 && bytes[1] == 0 && (bytes[2] == 1 || (bytes[2] == 0 && bytes[3] == 1))) {
      unit.annex_b.insert(unit.annex_b.end(), bytes, bytes + s->size);
    } else {
      AppendStartCodeAndNal(unit.annex_b, bytes, s->size);
    }
  }
  vaUnmapBuffer(display_.display, enc_coded_);

  if (unit.annex_b.empty()) {
    return Error("H264 encode produced empty access unit");
  }

  enc_have_ref_ = true;
  enc_ref_slot_ = recon_slot;
  if (idr) {
    enc_frame_num_ = 1;
    enc_poc_ = 2;
    enc_frames_since_idr_ = 0;
    ++enc_idr_pic_id_;
  } else {
    ++enc_frame_num_;
    enc_poc_ += 2;
    ++enc_frames_since_idr_;
  }
  return unit;
}

Roe<void> VaapiVideoCodec::ConfigureDecoder() {
  if (!caps_.has_decode) {
    return Error("VA-API H264 decoder unavailable on this device");
  }
  decoder_configured_ = true;
  return {};
}

bool VaapiVideoCodec::EnsureDecoderContext(const ParsedSps& sps, std::string& error) {
  const int width = sps.coded_width > 0 ? sps.coded_width : Align16(sps.width);
  const int height = sps.coded_height > 0 ? sps.coded_height : Align16(sps.height);
  const VAProfile profile = SelectDecodeProfile(caps_, sps, dec_pps_);
  if (dec_context_ != VA_INVALID_ID && dec_width_ == width && dec_height_ == height &&
      dec_active_profile_ == profile) {
    return true;
  }

  // Tear down previous decode session.
  if (dec_context_ != VA_INVALID_ID) {
    vaDestroyContext(display_.display, dec_context_);
    dec_context_ = VA_INVALID_ID;
  }
  if (dec_config_ != VA_INVALID_ID) {
    vaDestroyConfig(display_.display, dec_config_);
    dec_config_ = VA_INVALID_ID;
  }
  for (VASurfaceID& s : dec_surfaces_) {
    if (s != VA_INVALID_ID) {
      vaDestroySurfaces(display_.display, &s, 1);
      s = VA_INVALID_ID;
    }
  }
  dec_have_ref_ = false;
  InvalidatePicture(dec_ref_);

  VAConfigAttrib attr{};
  attr.type = VAConfigAttribRTFormat;
  attr.value = VA_RT_FORMAT_YUV420;
  if (vaCreateConfig(display_.display, profile, VAEntrypointVLD, &attr, 1, &dec_config_) !=
      VA_STATUS_SUCCESS) {
    error = "vaCreateConfig (decode) failed";
    return false;
  }
  if (vaCreateSurfaces(display_.display, VA_RT_FORMAT_YUV420, static_cast<unsigned>(width),
                       static_cast<unsigned>(height), dec_surfaces_.data(), kMaxDpbSurfaces, nullptr,
                       0) != VA_STATUS_SUCCESS) {
    error = "vaCreateSurfaces (decode) failed";
    return false;
  }
  if (vaCreateContext(display_.display, dec_config_, width, height, VA_PROGRESSIVE,
                      dec_surfaces_.data(), kMaxDpbSurfaces, &dec_context_) != VA_STATUS_SUCCESS) {
    error = "vaCreateContext (decode) failed";
    return false;
  }
  dec_width_ = width;
  dec_height_ = height;
  dec_active_profile_ = profile;
  dec_next_surface_ = 0;
  return true;
}

Roe<VideoFrameRgba> VaapiVideoCodec::DecodeSliceAccessUnit(const std::vector<AnnexBNal>& nals) {
  std::vector<const AnnexBNal*> slice_nals;
  bool idr = false;
  for (const auto& nal : nals) {
    if (nal.type == 5) {
      if (!idr) {
        slice_nals.clear();
        idr = true;
      }
      slice_nals.push_back(&nal);
    } else if (nal.type == 1 && !idr) {
      slice_nals.push_back(&nal);
    }
  }
  if (slice_nals.empty()) {
    return Error("access unit contained no slice data");
  }
  if (!dec_have_sps_ || !dec_have_pps_) {
    return Error("H264 decoder waiting for SPS/PPS");
  }

  std::string err;
  if (!EnsureDecoderContext(dec_sps_, err)) {
    return Error(err);
  }

  struct SliceWork {
    const AnnexBNal* nal = nullptr;
    ParsedSliceHeader sh{};
  };
  std::vector<SliceWork> slices;
  slices.reserve(slice_nals.size());
  for (const AnnexBNal* nal : slice_nals) {
    const std::vector<uint8_t> rbsp = NalToRbsp(nal->data, nal->size);
    SliceWork work;
    work.nal = nal;
    if (!ParseSliceHeader(rbsp.data(), rbsp.size(), dec_sps_, dec_pps_, idr, work.sh)) {
      return Error("failed to parse H264 slice header");
    }
    slices.push_back(work);
  }

  const ParsedSliceHeader& sh0 = slices.front().sh;
  const VASurfaceID curr = dec_surfaces_[static_cast<size_t>(dec_next_surface_ % kMaxDpbSurfaces)];
  ++dec_next_surface_;

  int top_poc = sh0.pic_order_cnt_lsb;
  if (dec_sps_.pic_order_cnt_type == 2) {
    // Spec 8.2.1.3: POC derived from frame_num (no POC syntax in the slice header).
    top_poc = idr ? 0 : (2 * sh0.frame_num);
  }

  VAPictureParameterBufferH264 pic{};
  InvalidatePicture(pic.CurrPic);
  for (auto& r : pic.ReferenceFrames) {
    InvalidatePicture(r);
  }
  pic.CurrPic.picture_id = curr;
  pic.CurrPic.frame_idx = sh0.frame_num;
  pic.CurrPic.flags = VA_PICTURE_H264_SHORT_TERM_REFERENCE;
  pic.CurrPic.TopFieldOrderCnt = top_poc;
  pic.CurrPic.BottomFieldOrderCnt = top_poc;
  if (!idr && dec_have_ref_) {
    pic.ReferenceFrames[0] = dec_ref_;
  }
  pic.picture_width_in_mbs_minus1 = static_cast<uint16_t>(dec_sps_.pic_width_in_mbs_minus1);
  pic.picture_height_in_mbs_minus1 = static_cast<uint16_t>(dec_sps_.pic_height_in_map_units_minus1);
  pic.bit_depth_luma_minus8 = static_cast<uint8_t>(dec_sps_.bit_depth_luma_minus8);
  pic.bit_depth_chroma_minus8 = static_cast<uint8_t>(dec_sps_.bit_depth_chroma_minus8);
  pic.num_ref_frames = static_cast<uint8_t>(dec_sps_.max_num_ref_frames);
  pic.seq_fields.bits.chroma_format_idc = dec_sps_.chroma_format_idc;
  pic.seq_fields.bits.frame_mbs_only_flag = dec_sps_.frame_mbs_only_flag;
  pic.seq_fields.bits.direct_8x8_inference_flag = dec_sps_.direct_8x8_inference_flag;
  pic.seq_fields.bits.log2_max_frame_num_minus4 = dec_sps_.log2_max_frame_num_minus4;
  pic.seq_fields.bits.pic_order_cnt_type = dec_sps_.pic_order_cnt_type;
  pic.seq_fields.bits.log2_max_pic_order_cnt_lsb_minus4 = dec_sps_.log2_max_pic_order_cnt_lsb_minus4;
  pic.seq_fields.bits.delta_pic_order_always_zero_flag = dec_sps_.delta_pic_order_always_zero_flag;
  pic.pic_init_qp_minus26 = static_cast<int8_t>(dec_pps_.pic_init_qp_minus26);
  pic.chroma_qp_index_offset = static_cast<int8_t>(dec_pps_.chroma_qp_index_offset);
  pic.second_chroma_qp_index_offset = static_cast<int8_t>(dec_pps_.second_chroma_qp_index_offset);
  pic.pic_fields.bits.entropy_coding_mode_flag = dec_pps_.entropy_coding_mode_flag;
  pic.pic_fields.bits.weighted_pred_flag = dec_pps_.weighted_pred_flag;
  pic.pic_fields.bits.weighted_bipred_idc = dec_pps_.weighted_bipred_idc;
  pic.pic_fields.bits.transform_8x8_mode_flag = dec_pps_.transform_8x8_mode_flag;
  pic.pic_fields.bits.constrained_intra_pred_flag = dec_pps_.constrained_intra_pred_flag;
  pic.pic_fields.bits.pic_order_present_flag = dec_pps_.bottom_field_pic_order_in_frame_present_flag;
  pic.pic_fields.bits.deblocking_filter_control_present_flag =
      dec_pps_.deblocking_filter_control_present_flag;
  pic.pic_fields.bits.redundant_pic_cnt_present_flag = dec_pps_.redundant_pic_cnt_present_flag;
  pic.pic_fields.bits.reference_pic_flag = 1;
  pic.frame_num = static_cast<uint16_t>(sh0.frame_num);

  VAIQMatrixBufferH264 iq{};
  for (int i = 0; i < 6; ++i) {
    for (int j = 0; j < 16; ++j) {
      iq.ScalingList4x4[i][j] = 16;
    }
  }
  for (int i = 0; i < 2; ++i) {
    for (int j = 0; j < 64; ++j) {
      iq.ScalingList8x8[i][j] = 16;
    }
  }

  VABufferID pic_buf = VA_INVALID_ID;
  VABufferID iq_buf = VA_INVALID_ID;
  if (vaCreateBuffer(display_.display, dec_context_, VAPictureParameterBufferType, sizeof(pic), 1,
                     &pic, &pic_buf) != VA_STATUS_SUCCESS ||
      vaCreateBuffer(display_.display, dec_context_, VAIQMatrixBufferType, sizeof(iq), 1, &iq,
                     &iq_buf) != VA_STATUS_SUCCESS) {
    if (pic_buf != VA_INVALID_ID) {
      vaDestroyBuffer(display_.display, pic_buf);
    }
    if (iq_buf != VA_INVALID_ID) {
      vaDestroyBuffer(display_.display, iq_buf);
    }
    return Error("vaCreateBuffer (decode pic/iq) failed");
  }

  if (vaBeginPicture(display_.display, dec_context_, curr) != VA_STATUS_SUCCESS) {
    vaDestroyBuffer(display_.display, pic_buf);
    vaDestroyBuffer(display_.display, iq_buf);
    return Error("VA decode begin failed");
  }

  VABufferID header_bufs[2] = {pic_buf, iq_buf};
  if (vaRenderPicture(display_.display, dec_context_, header_bufs, 2) != VA_STATUS_SUCCESS) {
    vaDestroyBuffer(display_.display, pic_buf);
    vaDestroyBuffer(display_.display, iq_buf);
    (void)vaEndPicture(display_.display, dec_context_);
    return Error("VA decode render (pic) failed");
  }

  for (const SliceWork& work : slices) {
    VASliceParameterBufferH264 slice{};
    slice.slice_data_size = static_cast<uint32_t>(work.nal->size);
    slice.slice_data_offset = 0;
    slice.slice_data_flag = VA_SLICE_DATA_FLAG_ALL;
    slice.slice_data_bit_offset = static_cast<uint16_t>(work.sh.header_bit_size);
    slice.first_mb_in_slice = static_cast<uint16_t>(work.sh.first_mb_in_slice);
    slice.slice_type = static_cast<uint8_t>(work.sh.slice_type);
    slice.direct_spatial_mv_pred_flag = 0;
    slice.num_ref_idx_l0_active_minus1 = static_cast<uint8_t>(work.sh.num_ref_idx_l0_active_minus1);
    slice.num_ref_idx_l1_active_minus1 = static_cast<uint8_t>(work.sh.num_ref_idx_l1_active_minus1);
    slice.cabac_init_idc = static_cast<uint8_t>(work.sh.cabac_init_idc);
    slice.slice_qp_delta = static_cast<int8_t>(work.sh.slice_qp_delta);
    slice.disable_deblocking_filter_idc =
        static_cast<uint8_t>(work.sh.disable_deblocking_filter_idc);
    slice.slice_alpha_c0_offset_div2 = static_cast<int8_t>(work.sh.slice_alpha_c0_offset_div2);
    slice.slice_beta_offset_div2 = static_cast<int8_t>(work.sh.slice_beta_offset_div2);
    for (auto& r : slice.RefPicList0) {
      InvalidatePicture(r);
    }
    for (auto& r : slice.RefPicList1) {
      InvalidatePicture(r);
    }
    if (!idr && dec_have_ref_) {
      slice.RefPicList0[0] = dec_ref_;
    }

    VABufferID slice_bufs[2] = {VA_INVALID_ID, VA_INVALID_ID};
    if (vaCreateBuffer(display_.display, dec_context_, VASliceParameterBufferType, sizeof(slice), 1,
                       &slice, &slice_bufs[0]) != VA_STATUS_SUCCESS ||
        vaCreateBuffer(display_.display, dec_context_, VASliceDataBufferType,
                       static_cast<unsigned>(work.nal->size), 1,
                       const_cast<uint8_t*>(work.nal->data), &slice_bufs[1]) != VA_STATUS_SUCCESS) {
      for (VABufferID b : slice_bufs) {
        if (b != VA_INVALID_ID) {
          vaDestroyBuffer(display_.display, b);
        }
      }
      vaDestroyBuffer(display_.display, pic_buf);
      vaDestroyBuffer(display_.display, iq_buf);
      (void)vaEndPicture(display_.display, dec_context_);
      return Error("vaCreateBuffer (decode slice) failed");
    }
    if (vaRenderPicture(display_.display, dec_context_, slice_bufs, 2) != VA_STATUS_SUCCESS) {
      for (VABufferID b : slice_bufs) {
        vaDestroyBuffer(display_.display, b);
      }
      vaDestroyBuffer(display_.display, pic_buf);
      vaDestroyBuffer(display_.display, iq_buf);
      (void)vaEndPicture(display_.display, dec_context_);
      return Error("VA decode render (slice) failed");
    }
    for (VABufferID b : slice_bufs) {
      vaDestroyBuffer(display_.display, b);
    }
  }

  if (vaEndPicture(display_.display, dec_context_) != VA_STATUS_SUCCESS) {
    vaDestroyBuffer(display_.display, pic_buf);
    vaDestroyBuffer(display_.display, iq_buf);
    return Error("VA decode end failed");
  }
  vaDestroyBuffer(display_.display, pic_buf);
  vaDestroyBuffer(display_.display, iq_buf);

  VideoFrameRgba rgba;
  if (!DownloadSurfaceToRgba(display_.display, curr, dec_sps_.width, dec_sps_.height, rgba)) {
    return Error("failed to download decoded VA surface");
  }

  dec_ref_ = pic.CurrPic;
  dec_have_ref_ = true;
  return rgba;
}

Roe<VideoFrameRgba> VaapiVideoCodec::Decode(const uint8_t* annex_b, size_t size) {
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

  bool saw_slice = false;
  for (const auto& nal : nals) {
    if (nal.type == 7) {
      const std::vector<uint8_t> rbsp = NalToRbsp(nal.data, nal.size);
      ParsedSps sps{};
      if (!ParseSpsRbsp(rbsp.data(), rbsp.size(), sps)) {
        return Error("failed to parse H264 SPS");
      }
      dec_sps_ = sps;
      dec_have_sps_ = true;
    } else if (nal.type == 8) {
      const std::vector<uint8_t> rbsp = NalToRbsp(nal.data, nal.size);
      ParsedPps pps{};
      if (!ParsePpsRbsp(rbsp.data(), rbsp.size(), pps)) {
        return Error("failed to parse H264 PPS");
      }
      dec_pps_ = pps;
      dec_have_pps_ = true;
    } else if (nal.type == 1 || nal.type == 5) {
      saw_slice = true;
    }
  }
  if (!saw_slice) {
    return Error("access unit contained no slice data");
  }
  return DecodeSliceAccessUnit(nals);
}

void VaapiVideoCodec::ResetEncoder() {
  enc_ready_ = false;
  if (display_.display) {
    if (enc_coded_ != VA_INVALID_ID) {
      vaDestroyBuffer(display_.display, enc_coded_);
      enc_coded_ = VA_INVALID_ID;
    }
    if (enc_context_ != VA_INVALID_ID) {
      vaDestroyContext(display_.display, enc_context_);
      enc_context_ = VA_INVALID_ID;
    }
    VASurfaceID surfaces[3] = {enc_input_, enc_recon_[0], enc_recon_[1]};
    for (VASurfaceID& s : surfaces) {
      if (s != VA_INVALID_ID) {
        vaDestroySurfaces(display_.display, &s, 1);
        s = VA_INVALID_ID;
      }
    }
    enc_input_ = VA_INVALID_ID;
    enc_recon_[0] = VA_INVALID_ID;
    enc_recon_[1] = VA_INVALID_ID;
    if (enc_config_ != VA_INVALID_ID) {
      vaDestroyConfig(display_.display, enc_config_);
      enc_config_ = VA_INVALID_ID;
    }
  }
  enc_width_ = enc_height_ = enc_fps_ = 0;
  enc_have_ref_ = false;
}

void VaapiVideoCodec::ResetDecoder() {
  decoder_configured_ = false;
  dec_have_sps_ = false;
  dec_have_pps_ = false;
  dec_have_ref_ = false;
  InvalidatePicture(dec_ref_);
  if (display_.display) {
    if (dec_context_ != VA_INVALID_ID) {
      vaDestroyContext(display_.display, dec_context_);
      dec_context_ = VA_INVALID_ID;
    }
    for (VASurfaceID& s : dec_surfaces_) {
      if (s != VA_INVALID_ID) {
        vaDestroySurfaces(display_.display, &s, 1);
        s = VA_INVALID_ID;
      }
    }
    if (dec_config_ != VA_INVALID_ID) {
      vaDestroyConfig(display_.display, dec_config_);
      dec_config_ = VA_INVALID_ID;
    }
  }
  dec_width_ = dec_height_ = 0;
  dec_active_profile_ = VAProfileH264ConstrainedBaseline;
}

} // namespace

std::unique_ptr<IVideoCodec> CreateLinuxVideoCodec() {
  VaDisplayState display;
  VaCaps caps;
  std::string reason;
  if (!OpenFirstUsableDisplay(display, caps, reason)) {
    return MakeUnavailableVideoCodec(reason.empty() ? "Linux VA-API H264 unavailable" : reason);
  }
  auto codec = std::make_unique<VaapiVideoCodec>(std::move(display), caps);
  // Decoder-only hosts are OK (V017): HasEncoder stays false until ConfigureEncoder.
  // Encode capability is reflected after ConfigureEncoder succeeds.
  if (!codec->CapsHasEncoder() && !codec->CapsHasDecoder()) {
    return MakeUnavailableVideoCodec("VA-API H264 EncSlice/VLD unavailable");
  }
  // Pre-mark decoder availability so CallMediaEngine can ConfigureDecoder when
  // remote bitstream arrives; encoder remains unconfigured until Camera on.
  if (codec->CapsHasDecoder()) {
    (void)codec->ConfigureDecoder();
  }
  return codec;
}

} // namespace pbr

#else // !PP_BROWSER_HAS_LIBVA

namespace pbr {

std::unique_ptr<IVideoCodec> CreateLinuxVideoCodec() {
  return MakeUnavailableVideoCodec("Linux VA-API H264 not built (install libva-dev and reconfigure)");
}

} // namespace pbr

#endif // PP_BROWSER_HAS_LIBVA

#endif // defined(__linux__) && !defined(__ANDROID__)
