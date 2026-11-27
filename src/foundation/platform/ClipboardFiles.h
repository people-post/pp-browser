#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace pbr {

/**
 * Files copied in Finder / Explorer / a Linux file manager and now on the system clipboard (DEV/PLAN-desktop-drop-paste.md,
 * D2). The header is portable; each platform has its own implementation file (ClipboardFiles_<suffix>), registered in
 * CMakeLists.txt next to the other platform files. Main thread only.
 */

/** Cheap: asks the system only whether file addresses are on the clipboard, transfers no data. */
bool ClipboardHasFiles();

/** Absolute local paths, UTF-8, in the order the system gave them; empty when there are none or they cannot be read. */
std::vector<std::string> ReadClipboardFiles();

/**
 * The per-platform kill switch: true where ReadClipboardFiles is implemented. Flipping the constant in ONE platform's
 * ClipboardFiles_<suffix> file to false turns file paste off for that platform only: ClipboardHasFiles still says
 * "files", so the paste is left to the text field exactly as in D1, and nothing else changes.
 */
bool ClipboardFilePasteEnabled();

/**
 * Whether asking the system afresh "is an image of this type on the clipboard?" (SDL_HasClipboardData) is cheap here:
 * true on macOS (the pasteboard's canReadItem...) and Windows (IsClipboardFormatAvailable), where SDL's type list is a
 * cache refreshed only when the window gains focus, so a screenshot copied meanwhile is not in it; false elsewhere (X11
 * transfers the whole data to answer), where the type list is all the paste looks at.
 */
bool ClipboardImageQueryIsCheap();

/**
 * Portable and pure: the local files named by a text/uri-list body. One URI per line (CR, LF or CRLF), blank lines and
 * "#" comments skipped; only file:// with an empty host or "localhost" is accepted (percent-decoded as UTF-8 bytes);
 * every other scheme, a remote host, a relative path or a malformed percent sequence is dropped.
 */
std::vector<std::string> ParseUriListFiles(std::string_view uri_list);

} // namespace pbr
