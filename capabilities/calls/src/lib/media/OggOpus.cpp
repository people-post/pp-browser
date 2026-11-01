#include "lib/media/OggOpus.h"

#include <opus.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <string>

namespace pbr {
namespace {

constexpr uint8_t kFlagContinued = 0x01;
constexpr uint8_t kFlagBegin = 0x02;
constexpr uint8_t kFlagEnd = 0x04;
constexpr size_t kPageHeaderBytes = 27;
constexpr size_t kOpusHeadBytes = 19;
/** The comment header may be longer than an audio packet; this is still far below anything useful to a sender. */
constexpr size_t kMaxTagsBytes = 16 * 1024;
constexpr size_t kPacketsPerPage = 50; // one second of 20 ms packets
constexpr size_t kMaxSegments = 255;

const std::array<uint32_t, 256>& CrcTable() {
  static const std::array<uint32_t, 256> table = [] {
    std::array<uint32_t, 256> t{};
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t r = i << 24;
      for (int bit = 0; bit < 8; ++bit) {
        r = (r & 0x80000000u) ? ((r << 1) ^ 0x04C11DB7u) : (r << 1);
      }
      t[i] = r;
    }
    return t;
  }();
  return table;
}

uint16_t ReadLe16(const uint8_t* p) {
  return static_cast<uint16_t>(p[0] | (p[1] << 8));
}
uint32_t ReadLe32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}
uint64_t ReadLe64(const uint8_t* p) {
  return static_cast<uint64_t>(ReadLe32(p)) | (static_cast<uint64_t>(ReadLe32(p + 4)) << 32);
}

void PutLe16(std::vector<uint8_t>& out, uint16_t v) {
  out.push_back(static_cast<uint8_t>(v));
  out.push_back(static_cast<uint8_t>(v >> 8));
}
void PutLe32(std::vector<uint8_t>& out, uint32_t v) {
  for (int shift = 0; shift < 32; shift += 8) {
    out.push_back(static_cast<uint8_t>(v >> shift));
  }
}
void PutLe64(std::vector<uint8_t>& out, uint64_t v) {
  for (int shift = 0; shift < 64; shift += 8) {
    out.push_back(static_cast<uint8_t>(v >> shift));
  }
}

/** The lacing values of one packet: 255 for every full 255 bytes, then the rest (0 when it ends on a multiple). */
void AppendLacing(std::vector<uint8_t>& lacing, size_t packet_bytes) {
  size_t left = packet_bytes;
  while (left >= 255) {
    lacing.push_back(255);
    left -= 255;
  }
  lacing.push_back(static_cast<uint8_t>(left));
}

size_t LacingCount(size_t packet_bytes) {
  return packet_bytes / 255 + 1;
}

void AppendPage(std::vector<uint8_t>& out, uint32_t serial, uint32_t sequence, uint8_t flags, uint64_t granule,
                const std::vector<uint8_t>& lacing, const std::vector<uint8_t>& body) {
  const size_t start = out.size();
  const char magic[] = {'O', 'g', 'g', 'S'};
  out.insert(out.end(), magic, magic + 4);
  out.push_back(0); // version
  out.push_back(flags);
  PutLe64(out, granule);
  PutLe32(out, serial);
  PutLe32(out, sequence);
  PutLe32(out, 0); // checksum, filled in below
  out.push_back(static_cast<uint8_t>(lacing.size()));
  out.insert(out.end(), lacing.begin(), lacing.end());
  out.insert(out.end(), body.begin(), body.end());
  const uint32_t crc = OggCrc32(out.data() + start, out.size() - start);
  for (int i = 0; i < 4; ++i) {
    out[start + 22 + static_cast<size_t>(i)] = static_cast<uint8_t>(crc >> (8 * i));
  }
}

} // namespace

uint32_t OggCrc32(const uint8_t* data, size_t size, uint32_t crc) {
  const auto& table = CrcTable();
  for (size_t i = 0; i < size; ++i) {
    crc = (crc << 8) ^ table[((crc >> 24) & 0xFF) ^ data[i]];
  }
  return crc;
}

std::vector<uint8_t> BuildOggOpus(const std::vector<std::vector<uint8_t>>& packets, const uint64_t valid_samples,
                                  const OggOpusWriteParams& params) {
  std::vector<uint8_t> out;
  uint32_t sequence = 0;

  std::vector<uint8_t> head = {'O', 'p', 'u', 's', 'H', 'e', 'a', 'd', 1, 1};
  PutLe16(head, static_cast<uint16_t>(params.pre_skip));
  PutLe32(head, params.input_rate);
  PutLe16(head, 0); // output gain
  head.push_back(0); // mapping family 0: mono or stereo, no table
  AppendPage(out, params.serial, sequence++, kFlagBegin, 0, {static_cast<uint8_t>(head.size())}, head);

  const std::string vendor = "pp-browser";
  std::vector<uint8_t> tags = {'O', 'p', 'u', 's', 'T', 'a', 'g', 's'};
  PutLe32(tags, static_cast<uint32_t>(vendor.size()));
  tags.insert(tags.end(), vendor.begin(), vendor.end());
  PutLe32(tags, 0); // no comments
  AppendPage(out, params.serial, sequence++, packets.empty() ? kFlagEnd : 0, 0, {static_cast<uint8_t>(tags.size())},
             tags);

  const uint64_t granule_limit = static_cast<uint64_t>(params.pre_skip) + valid_samples;
  uint64_t samples_done = 0;
  size_t index = 0;
  while (index < packets.size()) {
    std::vector<uint8_t> lacing;
    std::vector<uint8_t> body;
    size_t in_page = 0;
    while (index < packets.size() && in_page < kPacketsPerPage &&
           lacing.size() + LacingCount(packets[index].size()) <= kMaxSegments) {
      AppendLacing(lacing, packets[index].size());
      body.insert(body.end(), packets[index].begin(), packets[index].end());
      ++index;
      ++in_page;
    }
    if (in_page == 0) {
      break; // a packet that cannot fit one page (not an Opus packet): stop rather than loop
    }
    samples_done += static_cast<uint64_t>(in_page) * kOggOpusFrameSamples;
    const bool last = index >= packets.size();
    const uint64_t granule = std::min<uint64_t>(params.pre_skip + samples_done, granule_limit);
    AppendPage(out, params.serial, sequence++, last ? kFlagEnd : 0, granule, lacing, body);
  }
  return out;
}

Roe<OggOpusStream> ParseOggOpus(const uint8_t* data, const size_t size, const OggOpusLimits& limits,
                                const std::function<bool()>& should_stop) {
  if (!data || size < kPageHeaderBytes) {
    return Error("ogg: too short");
  }
  if (size > limits.max_file_bytes) {
    return Error("ogg: file too large");
  }

  OggOpusStream stream;
  size_t pos = 0;
  size_t pages = 0;
  uint32_t serial = 0;
  uint32_t next_sequence = 0;
  bool ended = false;
  size_t packet_index = 0; // 0 head, 1 tags, then audio
  std::vector<uint8_t> current;
  bool current_open = false; // a packet continues onto the next page
  uint64_t total_samples = 0;
  bool have_granule = false;
  uint64_t last_granule = 0;

  while (pos < size) {
    if (++pages > limits.max_pages) {
      return Error("ogg: too many pages");
    }
    if (should_stop && should_stop()) {
      return Error("ogg: stopped");
    }
    if (ended) {
      return Error("ogg: data after the end of the stream");
    }
    if (size - pos < kPageHeaderBytes || std::memcmp(data + pos, "OggS", 4) != 0 || data[pos + 4] != 0) {
      return Error("ogg: bad page header");
    }
    const uint8_t flags = data[pos + 5];
    const uint64_t granule = ReadLe64(data + pos + 6);
    const uint32_t page_serial = ReadLe32(data + pos + 14);
    const uint32_t sequence = ReadLe32(data + pos + 18);
    const uint32_t stored_crc = ReadLe32(data + pos + 22);
    const size_t segments = data[pos + 26];
    if (size - pos < kPageHeaderBytes + segments) {
      return Error("ogg: truncated page");
    }
    const uint8_t* lacing = data + pos + kPageHeaderBytes;
    size_t body_bytes = 0;
    for (size_t i = 0; i < segments; ++i) {
      body_bytes += lacing[i];
    }
    const size_t page_bytes = kPageHeaderBytes + segments + body_bytes;
    if (size - pos < page_bytes) {
      return Error("ogg: truncated page body");
    }
    // The checksum covers the page with its own field read as zero.
    uint32_t crc = OggCrc32(data + pos, 22);
    const uint8_t zeros[4] = {0, 0, 0, 0};
    crc = OggCrc32(zeros, 4, crc);
    crc = OggCrc32(data + pos + 26, page_bytes - 26, crc);
    if (crc != stored_crc) {
      return Error("ogg: checksum mismatch");
    }

    if (pages == 1) {
      if (!(flags & kFlagBegin) || (flags & kFlagContinued)) {
        return Error("ogg: stream does not begin");
      }
      serial = page_serial;
    } else if ((flags & kFlagBegin) || page_serial != serial) {
      return Error("ogg: more than one stream");
    }
    if (sequence != next_sequence) {
      return Error("ogg: page out of order");
    }
    ++next_sequence;
    if (((flags & kFlagContinued) != 0) != current_open) {
      return Error("ogg: bad packet continuation");
    }

    const uint8_t* body = lacing + segments;
    size_t offset = 0;
    bool completed_a_packet = false;
    for (size_t i = 0; i < segments; ++i) {
      const size_t length = lacing[i];
      const size_t cap = packet_index == 1 ? kMaxTagsBytes : limits.max_packet_bytes;
      if (current.size() + length > cap) {
        return Error("ogg: packet too large");
      }
      current.insert(current.end(), body + offset, body + offset + length);
      offset += length;
      current_open = true;
      if (length == 255) {
        continue;
      }
      // A packet is complete.
      current_open = false;
      completed_a_packet = true;
      if (packet_index == 0) {
        if (current.size() < kOpusHeadBytes || std::memcmp(current.data(), "OpusHead", 8) != 0) {
          return Error("ogg: not Opus");
        }
        if ((current[8] >> 4) != 0) {
          return Error("ogg: unsupported Opus version");
        }
        stream.channels = current[9];
        stream.pre_skip = ReadLe16(current.data() + 10);
        stream.input_rate = ReadLe32(current.data() + 12);
        if (stream.channels != 1 || current[18] != 0) {
          return Error("ogg: only mono is supported");
        }
        if (stream.pre_skip > 8 * kOggOpusFrameSamples) {
          return Error("ogg: pre-skip out of range");
        }
      } else if (packet_index == 1) {
        if (current.size() < 8 || std::memcmp(current.data(), "OpusTags", 8) != 0) {
          return Error("ogg: missing comment header");
        }
      } else {
        if (current.empty()) {
          return Error("ogg: empty audio packet");
        }
        const int samples = opus_packet_get_nb_samples(current.data(), static_cast<opus_int32>(current.size()),
                                                       kOggOpusRate);
        if (samples <= 0) {
          return Error("ogg: not an Opus packet");
        }
        total_samples += static_cast<uint64_t>(samples);
        if (stream.packets.size() >= limits.max_packets ||
            total_samples > limits.max_samples + static_cast<uint64_t>(stream.pre_skip)) {
          return Error("ogg: too long");
        }
        stream.packets.push_back(current);
      }
      ++packet_index;
      current.clear();
    }

    if (completed_a_packet && granule != ~0ULL) {
      if (have_granule && granule < last_granule) {
        return Error("ogg: granule position goes back");
      }
      have_granule = true;
      last_granule = granule;
    }
    if (flags & kFlagEnd) {
      if (current_open) {
        return Error("ogg: stream ends inside a packet");
      }
      ended = true;
    }
    pos += page_bytes;
  }

  if (current_open) {
    return Error("ogg: truncated packet");
  }
  if (packet_index < 3 || stream.packets.empty()) {
    return Error("ogg: no audio");
  }
  if (total_samples <= static_cast<uint64_t>(stream.pre_skip)) {
    return Error("ogg: no audio after the pre-skip");
  }
  uint64_t valid = total_samples - static_cast<uint64_t>(stream.pre_skip);
  if (have_granule && last_granule >= static_cast<uint64_t>(stream.pre_skip)) {
    valid = std::min<uint64_t>(valid, last_granule - static_cast<uint64_t>(stream.pre_skip));
  }
  if (valid == 0 || valid > limits.max_samples) {
    return Error("ogg: length out of range");
  }
  stream.valid_samples = valid;
  return stream;
}

} // namespace pbr
