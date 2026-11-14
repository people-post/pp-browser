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

RelayInboxCursors LoadRelayInboxCursors(const std::string& profile_data_dir, const std::string& account_id) {
  RelayInboxCursors cursors;
  if (profile_data_dir.empty() || account_id.empty()) {
    return cursors;
  }
  std::ifstream in(CursorPath(profile_data_dir));
  if (!in) {
    return cursors;
  }
  // An oversized file reads as unparseable (PB-063).
  const auto text = ReadBounded(in, kMaxProfileJsonFileBytes);
  const auto root = text ? TryParseObject(*text) : std::nullopt;
  if (!root) {
    return cursors;
  }
  auto stored_id = root->getString("account_id");
  if (!stored_id || *stored_id != account_id) {
    return cursors;
  }
  cursors.preferred = root->getString("cursor").value_or("");
  if (const Object* relays = root->getObject("relays")) {
    for (const auto& [origin, value] : relays->entries()) {
      if (auto cursor = asString(value); cursor && !origin.empty() && !cursor->empty()) {
        cursors.others[origin] = *cursor;
      }
    }
  }
  return cursors;
}

void SaveRelayInboxCursors(const std::string& profile_data_dir, const std::string& account_id,
                           const RelayInboxCursors& cursors) {
  if (profile_data_dir.empty() || account_id.empty()) {
    return;
  }
  std::error_code ec;
  std::filesystem::create_directories(profile_data_dir, ec);
  Object root;
  root.set("account_id", account_id);
  if (!cursors.preferred.empty()) {
    root.set("cursor", cursors.preferred);
  }
  if (!cursors.others.empty()) {
    Object relays;
    for (const auto& [origin, cursor] : cursors.others) {
      relays.set(origin, cursor);
    }
    root.set("relays", std::move(relays));
  }
  // Replaced whole, not synced: written at every sync, and a cursor lost to a crash only fetches again what the
  // inbox drops as already seen.
  (void)AtomicFileWrite::Write(CursorPath(profile_data_dir).string(), DumpJson(root), AtomicFileWrite::Sync::None);
}

} // namespace pbr
