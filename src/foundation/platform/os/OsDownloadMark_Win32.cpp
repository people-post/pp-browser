#include "foundation/platform/os/OsDownloadMark.h"

#include "common/PbrCompat.h"

#include <filesystem>
#include <fstream>

namespace pbr::os {

// Mark-of-the-Web: NTFS alternate data stream "<file>:Zone.Identifier", ZoneId=3 (Internet).
// Fails (reported to the caller) on file systems without streams, e.g. FAT.
Roe<void> MarkFileAsDownloaded(const std::string& path) {
  const std::filesystem::path stream(path + ":Zone.Identifier");
  std::ofstream out(stream, std::ios::binary | std::ios::trunc);
  if (!out) {
    return Error("Failed to open Zone.Identifier stream");
  }
  out << "[ZoneTransfer]\r\nZoneId=3\r\n";
  out.flush();
  if (!out) {
    return Error("Failed to write Zone.Identifier stream");
  }
  return {};
}

} // namespace pbr::os
