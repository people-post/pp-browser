#include "lib/voice/AudioContainerSniff.h"

#include <cstring>
#include <fstream>

namespace pbr {
namespace {

bool Has(const uint8_t* bytes, size_t n, size_t at, const char* tag) {
  const size_t len = std::strlen(tag);
  return n >= at + len && std::memcmp(bytes + at, tag, len) == 0;
}

} // namespace

AudioContainer SniffAudioContainer(const uint8_t* bytes, size_t n) {
  if (bytes == nullptr) {
    return AudioContainer::Unknown;
  }
  if (Has(bytes, n, 4, "ftyp")) {
    return AudioContainer::Mp4;
  }
  if (Has(bytes, n, 0, "RIFF") && Has(bytes, n, 8, "WAVE")) {
    return AudioContainer::Wav;
  }
  if (Has(bytes, n, 0, "fLaC")) {
    return AudioContainer::Flac;
  }
  if (Has(bytes, n, 0, "OggS")) {
    return AudioContainer::Ogg;
  }
  if (Has(bytes, n, 0, "FORM") && (Has(bytes, n, 8, "AIFF") || Has(bytes, n, 8, "AIFC"))) {
    return AudioContainer::Aiff;
  }
  if (Has(bytes, n, 0, "ID3")) {
    return AudioContainer::Mp3;
  }
  // An MPEG frame sync (11 set bits) then two layer bits: 00 is ADTS (AAC), anything else an MPEG audio layer.
  if (n >= 2 && bytes[0] == 0xFF && (bytes[1] & 0xE0) == 0xE0) {
    if ((bytes[1] & 0xF6) == 0xF0) {
      return AudioContainer::Aac;
    }
    if ((bytes[1] & 0x06) != 0) {
      return AudioContainer::Mp3;
    }
  }
  return AudioContainer::Unknown;
}

AudioContainer SniffAudioFile(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  uint8_t head[16] = {};
  in.read(reinterpret_cast<char*>(head), sizeof(head));
  const auto got = static_cast<size_t>(in.gcount());
  return got == 0 ? AudioContainer::Unknown : SniffAudioContainer(head, got);
}

const char* AudioContainerExtension(AudioContainer container) {
  switch (container) {
    case AudioContainer::Mp3:
      return "mp3";
    case AudioContainer::Mp4:
      return "m4a";
    case AudioContainer::Wav:
      return "wav";
    case AudioContainer::Flac:
      return "flac";
    case AudioContainer::Ogg:
      return "ogg";
    case AudioContainer::Aiff:
      return "aiff";
    case AudioContainer::Aac:
      return "aac";
    case AudioContainer::Unknown:
      break;
  }
  return nullptr;
}

} // namespace pbr
