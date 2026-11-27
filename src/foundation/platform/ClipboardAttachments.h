#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace pbr {

/**
 * What the system clipboard holds, by type only (DEV/PLAN-desktop-drop-paste.md, D1). Portable: SDL only, no platform
 * code. Main thread only (SDL's rule).
 */
struct ClipboardSummary {
  /** Copied files are on the clipboard (ClipboardHasFiles; a fake source: a file address type in the type list). */
  bool has_files = false;
  /** ClipboardFilePasteEnabled: when false, `has_files` only keeps the paste from being taken over (D1's behaviour). */
  bool files_enabled = false;
  /** A text type is advertised and, when a picture is there too, the text is not blank. */
  bool has_text = false;
  /** The picture type that can be read: "image/png" or empty. */
  std::string image_type;
  /** Some image type (image/...) is there but not a readable one (BMP, TIFF, ...). */
  bool has_unusable_image = false;
};

/** A picture read from the clipboard. */
struct ClipboardImage {
  std::string bytes;
  /** File extension without the dot. */
  std::string extension;
  /** The data is larger than the limit given: `bytes` is empty then. */
  bool too_large = false;
};

/** The three SDL calls this module needs, as a seam so tests can fake the clipboard. */
struct ClipboardSource {
  /** The types on offer (SDL_GetClipboardMimeTypes). */
  std::function<std::vector<std::string>()> mime_types;
  /** SDL_GetClipboardText. */
  std::function<std::string()> text;
  /** SDL_GetClipboardData(mime): nullopt when there is none. */
  std::function<std::optional<std::string>(const std::string& mime)> data;
  /** D2: ClipboardHasFiles / ReadClipboardFiles / ClipboardFilePasteEnabled. Unset (a test's fake) = files are judged by the type list, not enabled, not readable. */
  std::function<bool()> has_files;
  std::function<std::vector<std::string>()> read_files;
  std::function<bool()> files_enabled;
};

/** The real clipboard through SDL. */
ClipboardSource SdlClipboardSource();

/** Types only (the text is read only when a picture type is there as well; for files see ClipboardHasText). Never SDL_HasClipboardData. */
ClipboardSummary PeekClipboard(const ClipboardSource& source);
ClipboardSummary PeekClipboard();

/**
 * The PNG on the clipboard. nullopt: none, empty, or the bytes do not start with the PNG signature. `too_large` set:
 * more than `max_bytes`.
 */
std::optional<ClipboardImage> ReadClipboardImage(const ClipboardSource& source, size_t max_bytes);
std::optional<ClipboardImage> ReadClipboardImage(size_t max_bytes);

/** A text type is on the clipboard and the text is not blank (reads the text; call it only when it decides something). */
bool ClipboardHasText(const ClipboardSource& source);
bool ClipboardHasText();

/** The copied files' paths (D2); empty when the source has none. Main thread. */
std::vector<std::string> ReadClipboardFileList(const ClipboardSource& source);
std::vector<std::string> ReadClipboardFileList();

/** Tests: replaces the clipboard the no-argument calls use (an empty source = back to SDL). */
void SetClipboardSourceForTest(std::optional<ClipboardSource> source);

} // namespace pbr
