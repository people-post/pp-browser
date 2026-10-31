#include "lib/media/OggOpus.h"

#include <gtest/gtest.h>

#include <opus.h>

#include <cstring>
#include <fstream>
#include <iterator>
#include <string>

namespace pbr {
namespace {

std::vector<uint8_t> ReadFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

std::vector<uint8_t> ReferenceFile() {
  return ReadFile(std::string(PP_BROWSER_MEDIA_TEST_DATA_DIR) + "/voice_ref_1500ms.ogg");
}

/** n real Opus packets of 20 ms (silence encodes to tiny packets). */
std::vector<std::vector<uint8_t>> SilentPackets(const int n) {
  int err = 0;
  OpusEncoder* enc = opus_encoder_create(48000, 1, OPUS_APPLICATION_VOIP, &err);
  std::vector<int16_t> pcm(kOggOpusFrameSamples, 0);
  std::vector<std::vector<uint8_t>> packets;
  for (int i = 0; i < n; ++i) {
    unsigned char out[1275];
    const int bytes = opus_encode(enc, pcm.data(), kOggOpusFrameSamples, out, sizeof(out));
    packets.emplace_back(out, out + bytes);
  }
  opus_encoder_destroy(enc);
  return packets;
}

TEST(OggCrc, KnownVectors) {
  const auto crc = [](const std::string& s) { return OggCrc32(reinterpret_cast<const uint8_t*>(s.data()), s.size()); };
  // Computed with an independent bitwise implementation (poly 0x04C11DB7, init 0, no reflection, no final xor).
  EXPECT_EQ(crc(""), 0x00000000u);
  EXPECT_EQ(crc("a"), 0xA864DB20u);
  EXPECT_EQ(crc("OggS"), 0x5FB0A94Fu);
  EXPECT_EQ(crc("123456789"), 0x89A1897Fu);
  std::string all;
  for (int i = 0; i < 256; ++i) {
    all.push_back(static_cast<char>(i));
  }
  EXPECT_EQ(crc(all), 0xAC148725u);
}

TEST(OggCrc, ContinuesAcrossPieces) {
  const std::string s = "123456789";
  const auto* p = reinterpret_cast<const uint8_t*>(s.data());
  EXPECT_EQ(OggCrc32(p + 4, 5, OggCrc32(p, 4)), OggCrc32(p, 9));
}

TEST(OggCrc, MatchesTheChecksumAnOtherMuxerWrote) {
  // The first page of the ffmpeg-made reference file stores 0x271BAD4F; the parser checks every page's checksum.
  const auto file = ReferenceFile();
  ASSERT_GT(file.size(), 47u);
  EXPECT_EQ(static_cast<uint32_t>(file[22]) | (file[23] << 8) | (file[24] << 16) | (static_cast<uint32_t>(file[25]) << 24),
            0x271BAD4Fu);
  std::vector<uint8_t> page(file.begin(), file.begin() + 47);
  std::memset(page.data() + 22, 0, 4);
  EXPECT_EQ(OggCrc32(page.data(), page.size()), 0x271BAD4Fu);
}

TEST(OggOpusParse, ReadsTheFfmpegReferenceFile) {
  // voice_ref_1500ms.ogg: `ffmpeg -f lavfi -i sine=frequency=440:duration=1.5:sample_rate=48000 -ac 1 -c:a libopus
  //   -b:a 24k -application voip -frame_duration 20` (ffmpeg 8.1, libopus). Regenerate the same way.
  const auto file = ReferenceFile();
  ASSERT_FALSE(file.empty());
  auto stream = ParseOggOpus(file.data(), file.size());
  ASSERT_TRUE(stream) << stream.error().message;
  EXPECT_EQ(stream->channels, 1);
  EXPECT_EQ(stream->pre_skip, 312);
  EXPECT_EQ(stream->input_rate, 48000u);
  EXPECT_EQ(stream->packets.size(), 76u);       // 75 frames of audio + the one that flushes the decoder delay
  EXPECT_EQ(stream->valid_samples, 72000u);     // the end granule (73312) minus the pre-skip: exactly 1.5 s
  EXPECT_EQ(stream->DurationMs(), 1500);
}

TEST(OggOpusParse, ReferenceFileDecodesToItsLength) {
  const auto file = ReferenceFile();
  auto stream = ParseOggOpus(file.data(), file.size());
  ASSERT_TRUE(stream);
  int err = 0;
  OpusDecoder* dec = opus_decoder_create(48000, 1, &err);
  ASSERT_EQ(err, OPUS_OK);
  size_t samples = 0;
  std::vector<int16_t> pcm(5760);
  for (const auto& packet : stream->packets) {
    const int n = opus_decode(dec, packet.data(), static_cast<opus_int32>(packet.size()), pcm.data(), 5760, 0);
    ASSERT_GT(n, 0);
    samples += static_cast<size_t>(n);
  }
  opus_decoder_destroy(dec);
  EXPECT_EQ(samples - static_cast<size_t>(stream->pre_skip), 76u * 960u - 312u);
  EXPECT_GE(samples - static_cast<size_t>(stream->pre_skip), stream->valid_samples);
}

TEST(OggOpusWrite, RoundTripsThroughTheParser) {
  const auto packets = SilentPackets(120); // 2.4 s: several pages
  OggOpusWriteParams params;
  const std::vector<uint8_t> file = BuildOggOpus(packets, 120u * 960u - 312u - 100u, params);
  auto stream = ParseOggOpus(file.data(), file.size());
  ASSERT_TRUE(stream) << stream.error().message;
  EXPECT_EQ(stream->packets, packets);
  EXPECT_EQ(stream->pre_skip, 312);
  EXPECT_EQ(stream->valid_samples, 120u * 960u - 312u - 100u); // the end granule trims the padding
  EXPECT_EQ(std::memcmp(file.data(), "OggS", 4), 0);
}

TEST(OggOpusWrite, FileStartsWithTheHeadersAndEndsWithEos) {
  const auto packets = SilentPackets(3);
  const auto file = BuildOggOpus(packets, 3 * 960 - 312);
  ASSERT_GT(file.size(), 100u);
  EXPECT_EQ(file[5], 0x02);                    // first page: begin of stream
  EXPECT_EQ(std::memcmp(file.data() + 28, "OpusHead", 8), 0);
  // Last page carries the end-of-stream flag; find it by walking the pages.
  size_t pos = 0;
  uint8_t last_flags = 0;
  while (pos < file.size()) {
    last_flags = file[pos + 5];
    size_t body = 0;
    for (size_t i = 0; i < file[pos + 26]; ++i) {
      body += file[pos + 27 + i];
    }
    pos += 27 + file[pos + 26] + body;
  }
  EXPECT_EQ(pos, file.size());
  EXPECT_EQ(last_flags & 0x04, 0x04);
}

TEST(OggOpusWrite, ALongPacketSpansSeveralLacingValues) {
  std::vector<std::vector<uint8_t>> packets = SilentPackets(2);
  packets[0].assign(1000, packets[0][0]);  // not decodable audio, but framing must carry it intact
  const auto file = BuildOggOpus(packets, 2 * 960 - 312);
  // Framing only: read the pages back by hand and count the bytes of packet 0 from the lacing.
  size_t pos = 0;
  int page = 0;
  size_t third_page_first_packet = 0;
  while (pos < file.size()) {
    const size_t segs = file[pos + 26];
    size_t body = 0;
    size_t first = 0;
    bool in_first = true;
    for (size_t i = 0; i < segs; ++i) {
      body += file[pos + 27 + i];
      if (in_first) {
        first += file[pos + 27 + i];
        in_first = file[pos + 27 + i] == 255;
      }
    }
    if (page == 2) {
      third_page_first_packet = first;
    }
    pos += 27 + segs + body;
    ++page;
  }
  EXPECT_EQ(third_page_first_packet, 1000u);
}

class OggOpusMalformed : public ::testing::Test {
protected:
  void SetUp() override {
    packets_ = SilentPackets(20);
    good_ = BuildOggOpus(packets_, 20u * 960u - 312u);
    ASSERT_TRUE(ParseOggOpus(good_.data(), good_.size()));
  }
  static void FixCrcOfPage(std::vector<uint8_t>& file, const size_t page_start) {
    size_t body = 0;
    for (size_t i = 0; i < file[page_start + 26]; ++i) {
      body += file[page_start + 27 + i];
    }
    const size_t len = 27 + file[page_start + 26] + body;
    std::memset(file.data() + page_start + 22, 0, 4);
    const uint32_t crc = OggCrc32(file.data() + page_start, len);
    for (int i = 0; i < 4; ++i) {
      file[page_start + 22 + static_cast<size_t>(i)] = static_cast<uint8_t>(crc >> (8 * i));
    }
  }
  std::vector<std::vector<uint8_t>> packets_;
  std::vector<uint8_t> good_;
};

TEST_F(OggOpusMalformed, EveryTruncationFailsWithoutCrashing) {
  for (size_t cut = 0; cut < good_.size(); ++cut) {
    auto parsed = ParseOggOpus(good_.data(), cut);
    EXPECT_FALSE(parsed) << "cut at " << cut;
  }
}

TEST_F(OggOpusMalformed, EverySingleByteFlipEitherFailsOrParsesCleanly) {
  // Most flips break a checksum; none may crash, hang or read outside the buffer (run under ASan in CI).
  for (size_t i = 0; i < good_.size(); ++i) {
    auto copy = good_;
    copy[i] ^= 0x5A;
    (void)ParseOggOpus(copy.data(), copy.size());
  }
  SUCCEED();
}

TEST_F(OggOpusMalformed, ABrokenChecksumIsRefused) {
  auto copy = good_;
  copy[copy.size() - 1] ^= 0x01;
  auto parsed = ParseOggOpus(copy.data(), copy.size());
  ASSERT_FALSE(parsed);
  EXPECT_NE(parsed.error().message.find("checksum"), std::string::npos);
}

TEST_F(OggOpusMalformed, NotOggAndEmptyAreRefused) {
  const std::string text = "this is not an ogg file, it is just text that is long enough to have a header";
  EXPECT_FALSE(ParseOggOpus(reinterpret_cast<const uint8_t*>(text.data()), text.size()));
  EXPECT_FALSE(ParseOggOpus(nullptr, 0));
  EXPECT_FALSE(ParseOggOpus(good_.data(), 0));
}

TEST_F(OggOpusMalformed, StereoIsRefused) {
  auto copy = good_;
  copy[28 + 9] = 2; // OpusHead channel count
  FixCrcOfPage(copy, 0);
  EXPECT_FALSE(ParseOggOpus(copy.data(), copy.size()));
}

TEST_F(OggOpusMalformed, ASecondStreamIsRefused) {
  auto copy = good_;
  const std::vector<uint8_t> again = good_;
  copy.insert(copy.end(), again.begin(), again.end()); // chained stream: begin page after the end
  EXPECT_FALSE(ParseOggOpus(copy.data(), copy.size()));
}

TEST_F(OggOpusMalformed, PagesOutOfOrderAreRefused) {
  auto copy = good_;
  // Swap the sequence numbers of page 2 and 3 (and fix their checksums).
  size_t starts[4] = {0, 0, 0, 0};
  size_t pos = 0;
  for (int p = 0; p < 4; ++p) {
    starts[p] = pos;
    size_t body = 0;
    for (size_t i = 0; i < copy[pos + 26]; ++i) {
      body += copy[pos + 27 + i];
    }
    pos += 27 + copy[pos + 26] + body;
  }
  copy[starts[2] + 18] = 3;
  copy[starts[3] + 18] = 2;
  FixCrcOfPage(copy, starts[2]);
  FixCrcOfPage(copy, starts[3]);
  EXPECT_FALSE(ParseOggOpus(copy.data(), copy.size()));
}

TEST_F(OggOpusMalformed, NotOpusPacketsAreRefused) {
  std::vector<std::vector<uint8_t>> junk(3, std::vector<uint8_t>{});
  junk[0] = {0xFF};            // a TOC byte that announces a frame count with no frames
  junk[1] = {0x03};            // code 3 (arbitrary frame count) with no count byte
  junk[2] = {};
  const auto file = BuildOggOpus(junk, 3 * 960 - 312);
  EXPECT_FALSE(ParseOggOpus(file.data(), file.size()));
}

TEST_F(OggOpusMalformed, LimitOnPackets) {
  OggOpusLimits limits;
  limits.max_packets = 10;
  EXPECT_FALSE(ParseOggOpus(good_.data(), good_.size(), limits));
  limits.max_packets = 20;
  EXPECT_TRUE(ParseOggOpus(good_.data(), good_.size(), limits));
}

TEST_F(OggOpusMalformed, LimitOnPages) {
  OggOpusLimits limits;
  limits.max_pages = 3; // head, tags, one audio page: 20 packets still fit one page, so this passes ...
  EXPECT_TRUE(ParseOggOpus(good_.data(), good_.size(), limits));
  limits.max_pages = 2; // ... and two pages is not enough for the headers plus any audio
  EXPECT_FALSE(ParseOggOpus(good_.data(), good_.size(), limits));
}

TEST_F(OggOpusMalformed, LimitOnDuration) {
  OggOpusLimits limits;
  limits.max_samples = 10u * 960u; // the file holds 20 frames
  auto parsed = ParseOggOpus(good_.data(), good_.size(), limits);
  EXPECT_FALSE(parsed);
}

TEST_F(OggOpusMalformed, LimitOnPacketSize) {
  OggOpusLimits limits;
  limits.max_packet_bytes = 1;
  EXPECT_FALSE(ParseOggOpus(good_.data(), good_.size(), limits));
}

TEST_F(OggOpusMalformed, LimitOnFileSize) {
  OggOpusLimits limits;
  limits.max_file_bytes = good_.size() - 1;
  EXPECT_FALSE(ParseOggOpus(good_.data(), good_.size(), limits));
}

TEST_F(OggOpusMalformed, ManyEmptyPagesStopAtThePageLimit) {
  // A hostile file of valid but empty pages must hit the page limit instead of being walked to the end.
  size_t head_tags_end = 0;
  for (int p = 0; p < 2; ++p) {
    size_t body = 0;
    for (size_t i = 0; i < good_[head_tags_end + 26]; ++i) {
      body += good_[head_tags_end + 27 + i];
    }
    head_tags_end += 27 + good_[head_tags_end + 26] + body;
  }
  std::vector<uint8_t> file(good_.begin(), good_.begin() + static_cast<std::ptrdiff_t>(head_tags_end));
  for (uint32_t seq = 2; seq < 2 + 500; ++seq) {
    const size_t start = file.size();
    const uint8_t header[27] = {'O', 'g', 'g', 'S', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                file[14], file[15], file[16], file[17],
                                static_cast<uint8_t>(seq), static_cast<uint8_t>(seq >> 8), 0, 0, 0, 0, 0, 0, 0};
    file.insert(file.end(), header, header + 27);
    FixCrcOfPage(file, start);
  }
  OggOpusLimits limits;
  limits.max_pages = 100;
  auto parsed = ParseOggOpus(file.data(), file.size(), limits);
  ASSERT_FALSE(parsed);
  EXPECT_NE(parsed.error().message.find("too many pages"), std::string::npos);
}

} // namespace
} // namespace pbr
