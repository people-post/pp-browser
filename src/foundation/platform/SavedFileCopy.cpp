#include "foundation/platform/SavedFileCopy.h"

#include "common/PathUtf8.h"

#include <filesystem>
#include <fstream>
#include <vector>

namespace pbr {

Roe<void> CopyFileInChunks(const std::string& source, const std::string& target, const ChunkWriteHook& write_hook) {
  const std::filesystem::path to = PathFromUtf8(target);
  if (to.filename().empty()) {
    return Error("No file name to save under");
  }
  std::ifstream in(PathFromUtf8(source), std::ios::binary);
  if (!in) {
    return Error("Cannot read the file to save");
  }
  const std::filesystem::path part = to.parent_path() / PathFromUtf8("." + PathToUtf8(to.filename()) + ".pp-part");
  bool ok = true;
  {
    std::ofstream out(part, std::ios::binary | std::ios::trunc);
    if (!out) {
      return Error("Cannot write in the chosen folder"); // nothing was created
    }
    std::vector<char> buffer(kSavedFileChunkBytes);
    for (size_t index = 0;; ++index) {
      in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
      const std::streamsize got = in.gcount();
      if (got <= 0) {
        break;
      }
      if (write_hook && !write_hook(index)) {
        ok = false;
        break;
      }
      if (!out.write(buffer.data(), got)) {
        ok = false;
        break;
      }
    }
    ok = ok && !in.bad();
    out.close();
    ok = ok && !out.fail();
  }
  std::error_code ec;
  if (ok) {
    std::filesystem::rename(part, to, ec); // replaces a file already there only now, when the copy is whole
    ok = !ec;
  }
  if (!ok) {
    std::error_code ignored;
    std::filesystem::remove(part, ignored);
    return Error("Could not save the file");
  }
  return {};
}

} // namespace pbr
