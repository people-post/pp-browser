#include "lib/net/RelayInboxCursor.h"
#include "common/AtomicFileWrite.h"
#include "common/PlatformLimits.h"
#include "common/BoundedRead.h"

#include "common/ValueJson.h"

#include <fstream>
#include <filesystem>
#include <sstream>
#include "common/PbrCompat.h"

namespace pbr {
namespace {

std::filesystem::path CursorPath(const std::string& profile_data_dir) {
  return std::filesystem::path(profile_data_dir) / "relay_inbox_cursor.json";
}

} // namespace

std::string LoadRelayInboxCursor(const std::string& profile_data_dir, const std::string& relay_user_id) {
  if (profile_data_dir.empty() || relay_user_id.empty()) {
    return {};
  }
  std::ifstream in(CursorPath(profile_data_dir));
  if (!in) {
    return {};
  }
  // An oversized file reads as unparseable (PB-063).
  const auto text = ReadBounded(in, kMaxProfileJsonFileBytes);
  const auto root = text ? TryParseObject(*text) : std::nullopt;
  if (!root) {
    return {};
  }
  auto stored_id = root->getString("relay_user_id");
  if (!stored_id || *stored_id != relay_user_id) {
    return {};
  }
  return root->getString("cursor").value_or("");
}

void SaveRelayInboxCursor(const std::string& profile_data_dir, const std::string& relay_user_id,
                          const std::string& cursor) {
  if (profile_data_dir.empty() || relay_user_id.empty() || cursor.empty()) {
    return;
  }
  std::error_code ec;
  std::filesystem::create_directories(profile_data_dir, ec);
  Object root;
  root.set("relay_user_id", relay_user_id);
  root.set("cursor", cursor);
  // Replaced whole, not synced: written at every sync, and a cursor lost to a crash only fetches again what the
  // inbox drops as already seen.
  (void)AtomicFileWrite::Write(CursorPath(profile_data_dir).string(), DumpJson(root), AtomicFileWrite::Sync::None);
}

} // namespace pbr
