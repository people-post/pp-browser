#include "foundation/platform/SavedFileCopy.h"

#include "common/PathUtf8.h"
#include "common/Utilities.h"

#include <filesystem>
#include <fstream>
#include <vector>

namespace pbr {

Roe<void> CopyFileInChunks(const std::string& source, const std::string& target, const ChunkWriteHook& write_hook) {
  std::filesystem::path to = PathFromUtf8(target);
  if (to.filename().empty()) {
    return Error("No file name to save under");
  }
  // A target that is a link to a file: the file behind it is replaced, the link stays (a dangling link: the link is).
  std::error_code link_ec;
  if (std::filesystem::is_symlink(to, link_ec)) {
    const std::filesystem::path resolved = std::filesystem::canonical(to, link_ec);
    if (!link_ec) {
      to = resolved;
    }
  }
  std::ifstream in(PathFromUtf8(source), std::ios::binary);
  if (!in) {
    return Error("Cannot read the file to save");
  }
  // A short fixed-form name: the user's name may already be as long as the file system allows.
  const std::filesystem::path part = to.parent_path() / PathFromUtf8(".pp-part-" + util::GenerateUuid().substr(0, 8));
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
