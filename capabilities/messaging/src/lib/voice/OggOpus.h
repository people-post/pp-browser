#pragma once

#include "common/Error.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * Ogg framing for one mono Opus stream (RFC 3533 pages, RFC 7845 Opus mapping): the muxer for voice
 * messages and the parser that reads one back. The parser takes a file a stranger sent, so every
 * count it reads is bounded before anything is allocated for it.
 */

/** The Ogg page checksum: CRC-32, polynomial 0x04C11DB7, initial value 0, no reflection, no final xor. */
uint32_t OggCrc32(const uint8_t* data, size_t size, uint32_t crc = 0);

/** Opus always counts its granule position in 48 kHz samples, whatever the input rate was. */
inline constexpr int kOggOpusRate = 48000;
inline constexpr int kOggOpusFrameSamples = 960; // 20 ms

struct OggOpusWriteParams {
  int pre_skip = 312;       // decoder delay in samples, from the encoder (OPUS_GET_LOOKAHEAD)
  uint32_t input_rate = 48000;
  uint32_t serial = 0x50505643; // arbitrary; one stream per file
};

/**
 * Build a whole Ogg Opus file from 20 ms packets (every packet is kOggOpusFrameSamples long).
 * `valid_samples`: how many samples after the pre-skip are real audio; the end granule says so and
 * the decoder drops the padding of the last packet. One logical stream, mono, mapping family 0.
 */
std::vector<uint8_t> BuildOggOpus(const std::vector<std::vector<uint8_t>>& packets, uint64_t valid_samples,
                                  const OggOpusWriteParams& params = {});

/** What the parser refuses to go beyond. The defaults are for a voice message. */
struct OggOpusLimits {
  size_t max_file_bytes = 1000 * 1000;
  size_t max_pages = 20000;
  /** An Opus packet is at most 1275 bytes (RFC 6716 s3.2.1). */
  size_t max_packet_bytes = 1275;
  size_t max_packets = 12000;
  /** Decoded length (after the pre-skip), in 48 kHz samples. */
  uint64_t max_samples = 125ULL * kOggOpusRate;
};

struct OggOpusStream {
  int channels = 1;
  int pre_skip = 0;
  uint32_t input_rate = 0;
  /** The audio packets in order (the two header packets are not included). */
  std::vector<std::vector<uint8_t>> packets;
  /** Real samples after the pre-skip, as far as the packets and the end granule agree. */
  uint64_t valid_samples = 0;

  int64_t DurationMs() const { return static_cast<int64_t>(valid_samples * 1000 / kOggOpusRate); }
};

/**
 * Parse one Ogg Opus stream. Fails (never crashes) on a bad checksum, a lacing that does not add
 * up, more than one stream, a missing or wrong header, a packet that is not valid Opus, or any of
 * the `limits`. `should_stop` is asked once per page: a parse nobody wants any more ends with an error. Only mono is accepted: it is what is recorded and what the speaker path takes.
 */
Roe<OggOpusStream> ParseOggOpus(const uint8_t* data, size_t size, const OggOpusLimits& limits = {},
                                const std::function<bool()>& should_stop = {});

} // namespace pbr
