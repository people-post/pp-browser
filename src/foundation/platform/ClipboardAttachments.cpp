#include "foundation/platform/ClipboardAttachments.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <utility>

namespace pbr {
namespace {

std::optional<ClipboardSource>& TestSource() {
  static std::optional<ClipboardSource> source;
  return source;
}

ClipboardSource ActiveSource() {
  return TestSource() ? *TestSource() : SdlClipboardSource();
}

std::string Lower(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return text;
}

bool IsFileType(const std::string& type) {
  return type == "public.file-url" || type == "text/uri-list";
}

bool IsTextType(const std::string& type) {
  return type.rfind("text/plain", 0) == 0 || type == "UTF8_STRING" || type == "STRING" || type == "TEXT" ||
         type == "public.utf8-plain-text" || type == "public.plain-text";
}

bool IsBlank(const std::string& text) {
  return std::all_of(text.begin(), text.end(), [](unsigned char c) { return std::isspace(c) != 0; });
}

bool HasPngSignature(const std::string& bytes) {
  static constexpr unsigned char kSignature[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  return bytes.size() >= sizeof(kSignature) && std::memcmp(bytes.data(), kSignature, sizeof(kSignature)) == 0;
}

} // namespace

ClipboardSource SdlClipboardSource() {
  ClipboardSource source;
  source.mime_types = []() {
    std::vector<std::string> types;
    size_t count = 0;
    char** list = SDL_GetClipboardMimeTypes(&count);
    if (list) {
      for (size_t i = 0; i < count; ++i) {
        if (list[i]) {
          types.emplace_back(list[i]);
        }
      }
      SDL_free(list);
    }
    return types;
  };
  source.text = []() {
    std::string text;
    if (char* raw = SDL_GetClipboardText()) {
      text = raw;
      SDL_free(raw);
    }
    return text;
  };
  source.data = [](const std::string& mime) -> std::optional<std::string> {
    size_t size = 0;
    void* raw = SDL_GetClipboardData(mime.c_str(), &size);
    if (!raw) {
      return std::nullopt;
    }
    std::string bytes(static_cast<const char*>(raw), size);
    SDL_free(raw);
    return bytes;
  };
  return source;
}

ClipboardSummary PeekClipboard(const ClipboardSource& source) {
  ClipboardSummary summary;
  if (!source.mime_types) {
    return summary;
  }
  bool text_type = false;
  bool any_image = false;
  for (const std::string& type : source.mime_types()) {
    const std::string lower = Lower(type);
    if (IsFileType(type) || IsFileType(lower)) {
      summary.has_files = true;
    } else if (IsTextType(type) || IsTextType(lower)) {
      text_type = true;
    } else if (lower.rfind("image/", 0) == 0) {
      any_image = true;
      if (lower == "image/png") {
        summary.image_type = "image/png";
      }
    }
  }
  summary.has_unusable_image = any_image && summary.image_type.empty();
  if (!text_type) {
    return summary;
  }
  if (!any_image) {
    summary.has_text = true; // nothing to decide against: the text is not read
    return summary;
  }
  summary.has_text = source.text && !IsBlank(source.text());
  return summary;
}

ClipboardSummary PeekClipboard() {
  return PeekClipboard(ActiveSource());
}

std::optional<ClipboardImage> ReadClipboardImage(const ClipboardSource& source, const size_t max_bytes) {
  if (!source.data) {
    return std::nullopt;
  }
  std::optional<std::string> bytes = source.data("image/png");
  if (!bytes || bytes->empty()) {
    return std::nullopt;
  }
  ClipboardImage image;
  if (bytes->size() > max_bytes) {
    image.too_large = true;
    return image;
  }
  if (!HasPngSignature(*bytes)) {
    return std::nullopt;
  }
  image.bytes = std::move(*bytes);
  image.extension = "png";
  return image;
}

std::optional<ClipboardImage> ReadClipboardImage(const size_t max_bytes) {
  return ReadClipboardImage(ActiveSource(), max_bytes);
}

void SetClipboardSourceForTest(std::optional<ClipboardSource> source) {
  TestSource() = std::move(source);
}

} // namespace pbr
