#pragma once

#include "common/PbrCompat.h"

#include <cstddef>
#include <functional>
#include <string>

namespace pbr {

/** How much CopyFileInChunks holds in memory at a time. */
inline constexpr size_t kSavedFileChunkBytes = size_t{1} << 20;

/** Tests only: called before chunk `chunk_index` (0, 1, ...) is written; false makes that write fail. */
using ChunkWriteHook = std::function<bool(size_t chunk_index)>;

/**
 * Copies `source` to `target` (UTF-8 paths) one chunk at a time, never the whole file in memory. The bytes go to a
 * temporary name beside the target (`.<name>.pp-part`) and are renamed over the target only once all of them are
 * written, so a file the user chose to replace stays whole when the copy fails (a full disk, no permission, a missing
 * folder, an unplugged volume). On failure only that temporary file is removed. Blocking: call off the UI thread.
 */
Roe<void> CopyFileInChunks(const std::string& source, const std::string& target, const ChunkWriteHook& write_hook = {});

} // namespace pbr
