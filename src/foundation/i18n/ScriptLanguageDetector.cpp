#include "foundation/i18n/ScriptLanguageDetector.h"

#include <cstdint>

namespace pbr {
namespace {

bool IsHiragana(char32_t cp) {
  return cp >= 0x3040 && cp <= 0x309F;
}

bool IsKatakana(char32_t cp) {
  return (cp >= 0x30A0 && cp <= 0x30FF) || (cp >= 0x31F0 && cp <= 0x31FF);
}

bool IsHangul(char32_t cp) {
  return (cp >= 0xAC00 && cp <= 0xD7AF) || (cp >= 0x1100 && cp <= 0x11FF) || (cp >= 0x3130 && cp <= 0x318F);
}

bool IsCjkUnified(char32_t cp) {
  return (cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0x3400 && cp <= 0x4DBF);
}

bool IsTraditionalIndicator(char32_t cp) {
  switch (cp) {
  case 0x81FA: // 臺
  case 0x70BA: // 為
  case 0x9019: // 這
  case 0x8AAA: // 說
  case 0x570B: // 國
  case 0x9ABD: // 體
  case 0x7D93: // 經
  case 0x8655: // 處
  case 0x61C9: // 應
  case 0x958B: // 開
  case 0x95DC: // 關
  case 0x96FB: // 電
  case 0x8A71: // 話
  case 0x6642: // 時
  case 0x9593: // 間
  case 0x8207: // 與
  case 0x7121: // 無
  case 0x5462: // 呢
  case 0x55CE: // 嗎
    return true;
  default:
    return false;
  }
}

std::optional<char32_t> NextCodepoint(std::string_view text, size_t& index) {
  if (index >= text.size()) {
    return std::nullopt;
  }
  const unsigned char lead = static_cast<unsigned char>(text[index]);
  size_t width = 1;
  if (lead < 0x80) {
    width = 1;
  } else if ((lead & 0xE0) == 0xC0) {
    width = 2;
  } else if ((lead & 0xF0) == 0xE0) {
    width = 3;
  } else if ((lead & 0xF8) == 0xF0) {
    width = 4;
  } else {
    ++index;
    return std::nullopt;
  }
  if (index + width > text.size()) {
    index = text.size();
    return std::nullopt;
  }
  char32_t cp = 0;
  switch (width) {
  case 1:
    cp = lead;
    break;
  case 2:
    cp = ((lead & 0x1F) << 6) | (static_cast<unsigned char>(text[index + 1]) & 0x3F);
    break;
  case 3:
    cp = ((lead & 0x0F) << 12) | ((static_cast<unsigned char>(text[index + 1]) & 0x3F) << 6) |
         (static_cast<unsigned char>(text[index + 2]) & 0x3F);
    break;
  case 4:
    cp = ((lead & 0x07) << 18) | ((static_cast<unsigned char>(text[index + 1]) & 0x3F) << 12) |
         ((static_cast<unsigned char>(text[index + 2]) & 0x3F) << 6) | (static_cast<unsigned char>(text[index + 3]) & 0x3F);
    break;
  default:
    break;
  }
  index += width;
  return cp;
}

} // namespace

std::optional<std::string> DetectChatMessageLanguage(std::string_view utf8_text) {
  bool has_japanese = false;
  bool has_korean = false;
  bool has_traditional = false;
  bool has_cjk = false;

  size_t index = 0;
  while (index < utf8_text.size()) {
    const auto cp = NextCodepoint(utf8_text, index);
    if (!cp) {
      continue;
    }
    if (IsHiragana(*cp) || IsKatakana(*cp)) {
      has_japanese = true;
    }
    if (IsHangul(*cp)) {
      has_korean = true;
    }
    if (IsTraditionalIndicator(*cp)) {
      has_traditional = true;
    }
    if (IsCjkUnified(*cp)) {
      has_cjk = true;
    }
  }

  if (has_japanese) {
    return "ja";
  }
  if (has_korean) {
    return "ko";
  }
  if (has_traditional) {
    return "zh-Hant";
  }
  if (has_cjk) {
    return "zh-Hans";
  }
  return std::nullopt;
}

std::string ApplyLangAttribute(std::string_view opening_tag, std::string_view utf8_text) {
  const auto lang = DetectChatMessageLanguage(utf8_text);
  if (!lang) {
    return std::string(opening_tag);
  }
  std::string result(opening_tag);
  result += " lang=\"";
  result += *lang;
  result += '"';
  return result;
}

} // namespace pbr
