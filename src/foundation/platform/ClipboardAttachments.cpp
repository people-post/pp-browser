#include "foundation/platform/ClipboardAttachments.h"

#include "foundation/platform/ClipboardFiles.h"

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

bool HasJpegSignature(const std::string& bytes) {
  static constexpr unsigned char kSignature[3] = {0xFF, 0xD8, 0xFF};
  return bytes.size() >= sizeof(kSignature) && std::memcmp(bytes.data(), kSignature, sizeof(kSignature)) == 0;
}

/** The picture types the paste reads, in the order it prefers them. */
struct ReadableImage {
  const char* mime;
  const char* extension;
  bool (*has_signature)(const std::string&);
};
constexpr ReadableImage kReadableImages[] = {
    {"image/png", "png", &HasPngSignature},
    {"image/jpeg", "jpg", &HasJpegSignature},
};

/** Image types that are on the clipboard but cannot be read: asked about afresh, so a TIFF-only screenshot says so. */
constexpr const char* kUnreadableImageTypes[] = {"image/tiff", "image/bmp"};

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
  source.has_data = [](const std::string& mime) { return SDL_HasClipboardData(mime.c_str()); };
  source.fresh_query_cheap = []() { return ClipboardImageQueryIsCheap(); };
  source.has_files = []() { return ClipboardHasFiles(); };
  source.read_files = []() { return ReadClipboardFiles(); };
  source.files_enabled = []() { return ClipboardFilePasteEnabled(); };
  return source;
}

ClipboardSummary PeekClipboard(const ClipboardSource& source) {
  ClipboardSummary summary;
  if (!source.mime_types) {
    return summary;
  }
  bool text_type = false;
  bool any_image = false;
  std::vector<std::string> listed_images;
  for (const std::string& type : source.mime_types()) {
    const std::string lower = Lower(type);
    if (IsFileType(type) || IsFileType(lower)) {
      summary.has_files = true;
    } else if (IsTextType(type) || IsTextType(lower)) {
      text_type = true;
    } else if (lower.rfind("image/", 0) == 0) {
      any_image = true;
      listed_images.push_back(lower);
    }
  }
  // The picture types: asked afresh where that is cheap (the type list is a cache that a screenshot tool can outdate),
  // the list as the fallback hint; where it is not cheap, the list alone.
  const bool fresh = source.has_data && source.fresh_query_cheap && source.fresh_query_cheap();
  const auto listed = [&listed_images](const std::string& mime) {
    return std::find(listed_images.begin(), listed_images.end(), mime) != listed_images.end();
  };
  for (const ReadableImage& image : kReadableImages) {
    if (summary.image_type.empty() && (listed(image.mime) || (fresh && source.has_data(image.mime)))) {
      summary.image_type = image.mime;
      any_image = true;
    }
  }
  bool unreadable_found = false;
  if (fresh && summary.image_type.empty()) {
    for (const char* mime : kUnreadableImageTypes) {
      unreadable_found = unreadable_found || source.has_data(mime);
    }
  }
  any_image = any_image || unreadable_found;
  summary.has_unusable_image = any_image && summary.image_type.empty();
  if (source.has_files) {
    summary.has_files = source.has_files(); // the platform's own answer, not the type list's
  }
  summary.files_enabled = source.files_enabled && source.files_enabled();
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
  for (const ReadableImage& type : kReadableImages) {
    std::optional<std::string> bytes = source.data(type.mime);
    if (!bytes || bytes->empty()) {
      continue;
    }
    ClipboardImage image;
    if (bytes->size() > max_bytes) {
      image.too_large = true;
      return image;
    }
    if (!type.has_signature(*bytes)) {
      return std::nullopt;
    }
    image.bytes = std::move(*bytes);
    image.extension = type.extension;
    return image;
  }
  return std::nullopt;
}

std::optional<ClipboardImage> ReadClipboardImage(const size_t max_bytes) {
  return ReadClipboardImage(ActiveSource(), max_bytes);
}

bool ClipboardHasText(const ClipboardSource& source) {
  if (!source.mime_types || !source.text) {
    return false;
  }
  for (const std::string& type : source.mime_types()) {
    if (IsTextType(type) || IsTextType(Lower(type))) {
      return !IsBlank(source.text());
    }
  }
  return false;
}

bool ClipboardHasText() {
  return ClipboardHasText(ActiveSource());
}

std::vector<std::string> ReadClipboardFileList(const ClipboardSource& source) {
  return source.read_files ? source.read_files() : std::vector<std::string>();
}

std::vector<std::string> ReadClipboardFileList() {
  return ReadClipboardFileList(ActiveSource());
}

void SetClipboardSourceForTest(std::optional<ClipboardSource> source) {
  TestSource() = std::move(source);
}

} // namespace pbr
