#include "domain/people/AvatarGlyph.h"

#include <cstdint>

namespace pbr {
namespace {

std::string_view TrimAsciiWs(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\n' || s.front() == '\r')) {
    s.remove_prefix(1);
  }
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\n' || s.back() == '\r')) {
    s.remove_suffix(1);
  }
  return s;
}

/** Bytes in the first UTF-8 codepoint; 0 if invalid/empty. */
size_t Utf8CodepointByteLength(std::string_view s) {
  if (s.empty()) {
    return 0;
  }
  const auto lead = static_cast<unsigned char>(s[0]);
  if (lead < 0x80) {
    return 1;
  }
  if ((lead & 0xE0) == 0xC0) {
    return s.size() >= 2 ? 2 : 0;
  }
  if ((lead & 0xF0) == 0xE0) {
    return s.size() >= 3 ? 3 : 0;
  }
  if ((lead & 0xF8) == 0xF0) {
    return s.size() >= 4 ? 4 : 0;
  }
  return 0;
}

std::string FirstCodepoint(std::string_view s) {
  const size_t n = Utf8CodepointByteLength(s);
  if (n == 0) {
    return {};
  }
  std::string out(s.substr(0, n));
  if (n == 1) {
    const unsigned char c = static_cast<unsigned char>(out[0]);
    if (c >= 'a' && c <= 'z') {
      out[0] = static_cast<char>(c - 'a' + 'A');
    }
  }
  return out;
}

std::string_view StripIdentityPrefix(std::string_view id) {
  const size_t colon = id.find(':');
  if (colon != std::string_view::npos && colon + 1 < id.size()) {
    return id.substr(colon + 1);
  }
  return id;
}

bool IsAsciiAlnum(unsigned char c) {
  return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

std::string LetterFromStableId(std::string_view stable_id) {
  std::string_view rest = StripIdentityPrefix(TrimAsciiWs(stable_id));
  while (!rest.empty()) {
    const size_t n = Utf8CodepointByteLength(rest);
    if (n == 0) {
      break;
    }
    if (n == 1 && IsAsciiAlnum(static_cast<unsigned char>(rest[0]))) {
      return FirstCodepoint(rest);
    }
    if (n > 1) {
      // Non-ASCII codepoint in an id is rare; use it as the glyph.
      return FirstCodepoint(rest);
    }
    rest.remove_prefix(n);
  }
  return "?";
}

uint32_t Fnv1a32(std::string_view s) {
  uint32_t hash = 2166136261u;
  for (unsigned char c : s) {
    hash ^= c;
    hash *= 16777619u;
  }
  return hash;
}

} // namespace

std::string AvatarStableId(std::string_view account_id, std::string_view relay_id, std::string_view peer_id,
                           std::string_view contact_id) {
  if (!account_id.empty()) {
    return std::string(account_id);
  }
  if (!relay_id.empty()) {
    return std::string(relay_id);
  }
  if (!peer_id.empty()) {
    return std::string(peer_id);
  }
  if (!contact_id.empty()) {
    return std::string(contact_id);
  }
  return {};
}

AvatarGlyph MakeAvatarGlyph(std::string_view display_name, std::string_view stable_id) {
  AvatarGlyph out;
  const std::string_view name = TrimAsciiWs(display_name);
  if (!name.empty()) {
    out.letter = FirstCodepoint(name);
  }
  if (out.letter.empty()) {
    out.letter = LetterFromStableId(stable_id);
  }
  if (out.letter.empty()) {
    out.letter = "?";
  }

  std::string_view seed = TrimAsciiWs(stable_id);
  if (seed.empty()) {
    seed = name;
  }
  if (seed.empty()) {
    out.tone = 0;
  } else {
    out.tone = static_cast<int>(Fnv1a32(seed) % static_cast<uint32_t>(kAvatarToneCount));
  }
  return out;
}

} // namespace pbr
