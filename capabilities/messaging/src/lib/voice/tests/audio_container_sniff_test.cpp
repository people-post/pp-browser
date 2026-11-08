#include "lib/voice/AudioContainerSniff.h"

#include <gtest/gtest.h>

#include <cstring>
#include <string>

namespace pbr {
namespace {

AudioContainer Sniff(const std::string& bytes) {
  return SniffAudioContainer(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
}

TEST(AudioContainerSniff, KnownContainers) {
  EXPECT_EQ(Sniff("ID3\x04\x00\x00\x00\x00\x00\x00"), AudioContainer::Mp3);
  EXPECT_EQ(Sniff("\xFF\xFB\x90\x00"), AudioContainer::Mp3);
  EXPECT_EQ(Sniff("\xFF\xF3\x90\x00"), AudioContainer::Mp3);
  EXPECT_EQ(Sniff(std::string("\x00\x00\x00\x20", 4) + "ftypisom"), AudioContainer::Mp4);
  EXPECT_EQ(Sniff(std::string("RIFF\x24\x00\x00\x00", 8) + "WAVEfmt "), AudioContainer::Wav);
  EXPECT_EQ(Sniff("fLaC\x00\x00\x00\x22"), AudioContainer::Flac);
  EXPECT_EQ(Sniff("OggS\x00\x02"), AudioContainer::Ogg);
  EXPECT_EQ(Sniff(std::string("FORM\x00\x00\x00\x10", 8) + "AIFFCOMM"), AudioContainer::Aiff);
  EXPECT_EQ(Sniff(std::string("FORM\x00\x00\x00\x10", 8) + "AIFCCOMM"), AudioContainer::Aiff);
}

TEST(AudioContainerSniff, AdtsIsAacNotMp3) {
  EXPECT_EQ(Sniff("\xFF\xF1\x50\x80"), AudioContainer::Aac) << "ADTS, MPEG-4";
  EXPECT_EQ(Sniff("\xFF\xF9\x50\x80"), AudioContainer::Aac) << "ADTS, MPEG-2";
  EXPECT_EQ(Sniff("\xFF\xFB\x90\x00"), AudioContainer::Mp3) << "MPEG-1 Layer III";
  EXPECT_EQ(Sniff("ID3\x04\x00\x00\x00\x00\x00\x00"), AudioContainer::Mp3);
}

TEST(AudioContainerSniff, ShortAndUnknownBuffers) {
  EXPECT_EQ(SniffAudioContainer(nullptr, 0), AudioContainer::Unknown);
  EXPECT_EQ(Sniff(""), AudioContainer::Unknown);
  EXPECT_EQ(Sniff("\xFF"), AudioContainer::Unknown);
  EXPECT_EQ(Sniff("ID"), AudioContainer::Unknown);
  EXPECT_EQ(Sniff(std::string("\x00\x00\x00\x20", 4) + "fty"), AudioContainer::Unknown);
  EXPECT_EQ(Sniff("RIFF\x24\x00\x00\x00AVI LIST"), AudioContainer::Unknown) << "RIFF but not WAVE";
  EXPECT_EQ(Sniff("hello, this is text"), AudioContainer::Unknown);
  EXPECT_EQ(Sniff("\xFF\x10\x00"), AudioContainer::Unknown) << "no frame sync";
}

TEST(AudioContainerSniff, ExtensionNames) {
  EXPECT_STREQ(AudioContainerExtension(AudioContainer::Mp3), "mp3");
  EXPECT_STREQ(AudioContainerExtension(AudioContainer::Mp4), "m4a");
  EXPECT_STREQ(AudioContainerExtension(AudioContainer::Wav), "wav");
  EXPECT_STREQ(AudioContainerExtension(AudioContainer::Flac), "flac");
  EXPECT_STREQ(AudioContainerExtension(AudioContainer::Ogg), "ogg");
  EXPECT_STREQ(AudioContainerExtension(AudioContainer::Aiff), "aiff");
  EXPECT_STREQ(AudioContainerExtension(AudioContainer::Aac), "aac");
  EXPECT_EQ(AudioContainerExtension(AudioContainer::Unknown), nullptr);
}

} // namespace
} // namespace pbr
