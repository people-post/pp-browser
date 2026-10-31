#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <vector>

struct SDL_Window;

namespace pbr {

using NativeFileDialogCallback = std::function<void(std::vector<std::string> paths)>;

/**
 * Async native open dialog for a single image file. Empty paths = cancel. `include_heic` adds heic/heif to the
 * filter. The callback may run on any thread. On iOS the photo picker is used and the path is a private copy
 * under tmp/pp-picks/ (cleared on the next pick).
 */
void ShowOpenImageFileDialog(SDL_Window* window, NativeFileDialogCallback callback, bool include_heic = false);
void ShowOpenFileDialog(SDL_Window* window, NativeFileDialogCallback callback);
/**
 * The "Photo" entry of a chat: a picture or a video from the phone's library. iOS: PHPicker offering images and
 * videos; a video is kept as the library has it (.mov, HEVC) and a HEIC photo is handed over as JPEG. Android and
 * desktop: the system dialog filtered to pictures and video. Empty paths = cancel; same path rules as
 * ShowOpenImageFileDialog.
 */
void ShowOpenPhotoLibraryDialog(SDL_Window* window, NativeFileDialogCallback callback);
/**
 * An image kept as a file (not in the photo library): on iOS the Files picker limited to JPEG / PNG / GIF /
 * WebP; elsewhere the same dialog as ShowOpenImageFileDialog.
 */
void ShowOpenImageDocumentDialog(SDL_Window* window, NativeFileDialogCallback callback, bool include_heic = false);
/**
 * Done with a picked path: on iOS its private copy (tmp/pp-picks) is deleted, so a picked photo does not
 * stay on disk unencrypted. Anything else (desktop originals, other paths) is left alone.
 */
void ReleasePickedFile(const std::string& path);

/**
 * Save `bytes` as a file where the user chooses: the system save dialog offering `suggested_name` (desktop; Android's
 * create-document picker), the Files export picker on iOS. `done(saved)` — false on cancel or a failed write — may
 * run on any thread.
 */
void SaveFileAs(SDL_Window* window, const std::string& suggested_name, std::string bytes,
                std::function<void(bool saved)> done);

/** A picked file made readable by ordinary file calls (see ImportPickedFile). */
struct ImportedPick {
  /** What to read: the picked path itself, or the private copy of a content:// URI. */
  std::string path;
  /** The file's name when the pick says one (empty: unknown, name it from its content). */
  std::string name;
  /** `path` is a copy this call made: delete it with RemoveImportedPick when done. */
  bool copied = false;
};

/**
 * Android hands over content:// URIs that only SDL_IOFromFile (the JNI content resolver) can open; std::ifstream,
 * std::filesystem and image decoders cannot. A content:// pick is therefore streamed (1 MB at a time, never
 * the whole file in memory) into `{scratch_dir}/<unique>/<name>` and that copy is returned; any other path is
 * returned as it is. `force_copy` copies a plain path too (tests). nullopt when it cannot be read or written.
 */
std::optional<ImportedPick> ImportPickedFile(const std::string& picked, const std::string& scratch_dir,
                                             bool force_copy = false);
/** Delete the copy ImportPickedFile made (and its folder). */
void RemoveImportedPick(const ImportedPick& pick);

/** A picked file's bytes (a path, or an Android content:// URI); nullopt when unreadable or over `max_bytes`. */
std::optional<std::string> ReadPickedFile(const std::string& path, size_t max_bytes);

} // namespace pbr
