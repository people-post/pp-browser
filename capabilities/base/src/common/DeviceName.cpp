#include "common/DeviceName.h"

#include <vector>

namespace pbr {

namespace {

/** One strict UTF-8 decode (no overlong forms, no surrogates, nothing past U+10FFFF); false on bad input. */
bool DecodeOne(const std::string& s, size_t& i, char32_t& cp) {
  const auto byte = [&](size_t at) { return static_cast<unsigned char>(s[at]); };
  const unsigned char lead = byte(i);
  size_t extra = 0;
  char32_t min = 0;
  if (lead < 0x80) {
    cp = lead;
    ++i;
    return true;
  }
  if ((lead & 0xE0) == 0xC0) {
    extra = 1;
    cp = lead & 0x1F;
    min = 0x80;
  } else if ((lead & 0xF0) == 0xE0) {
    extra = 2;
    cp = lead & 0x0F;
    min = 0x800;
  } else if ((lead & 0xF8) == 0xF0) {
    extra = 3;
    cp = lead & 0x07;
    min = 0x10000;
  } else {
    return false;
  }
  if (i + extra >= s.size()) {
    return false;
  }
  for (size_t k = 1; k <= extra; ++k) {
    if ((byte(i + k) & 0xC0) != 0x80) {
      return false;
    }
    cp = (cp << 6) | (byte(i + k) & 0x3F);
  }
  if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
    return false;
  }
  i += extra + 1;
  return true;
}

void Append(std::string& out, const char32_t cp) {
  if (cp < 0x80) {
    out.push_back(static_cast<char>(cp));
  } else if (cp < 0x800) {
    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
}

bool IsControl(const char32_t cp) {
  return cp < 0x20 || (cp >= 0x7F && cp <= 0x9F) || cp == 0x2028 || cp == 0x2029;
}

bool IsSpace(const char32_t cp) {
  return cp == ' ' || cp == 0xA0 || cp == 0x3000;
}

void Trim(std::vector<char32_t>& cps) {
  while (!cps.empty() && IsSpace(cps.back())) {
    cps.pop_back();
  }
  size_t lead = 0;
  while (lead < cps.size() && IsSpace(cps[lead])) {
    ++lead;
  }
  cps.erase(cps.begin(), cps.begin() + static_cast<std::ptrdiff_t>(lead));
}

} // namespace

std::string NormalizeDeviceName(const std::string& raw) {
  std::vector<char32_t> cps;
  for (size_t i = 0; i < raw.size();) {
    char32_t cp = 0;
    if (!DecodeOne(raw, i, cp)) {
      return {};
    }
    if (!IsControl(cp)) {
      cps.push_back(cp);
    }
  }
  Trim(cps);
  if (cps.size() > kMaxDeviceNameChars) {
    cps.resize(kMaxDeviceNameChars);
    Trim(cps);
  }
  std::string out;
  for (const char32_t cp : cps) {
    Append(out, cp);
  }
  return out;
}

} // namespace pbr
