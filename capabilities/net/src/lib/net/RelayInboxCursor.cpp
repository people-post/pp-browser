#include "lib/net/RelayInboxCursor.h"
#include "common/AtomicFileWrite.h"
#include "common/PlatformLimits.h"
#include "common/BoundedRead.h"

#include "common/ValueJson.h"

#include <chrono>
#include <fstream>
#include <filesystem>
#include <limits>
#include <optional>
#include <sstream>
#include "common/PbrCompat.h"

namespace pbr {
namespace {

std::filesystem::path CursorPath(const std::string& profile_data_dir) {
  return std::filesystem::path(profile_data_dir) / "relay_inbox_cursor.json";
}

std::optional<Object> ReadCursorFile(const std::string& profile_data_dir) {
  std::ifstream in(CursorPath(profile_data_dir));
  if (!in) {
    return std::nullopt;
  }
  // An oversized file reads as unparseable (PB-063).
  const auto text = ReadBounded(in, kMaxProfileJsonFileBytes);
  return text ? TryParseObject(*text) : std::nullopt;
}

/** A newer app's file (INVENTORY S3): not read, and never written over. A file without a version is this one's. */
bool IsNewer(const Object& root) {
  return root.getNonNegInt("schema_version").value_or(kRelayInboxCursorSchemaVersion) > kRelayInboxCursorSchemaVersion;
}

} // namespace

RelayInboxCursors LoadRelayInboxCursors(const std::string& profile_data_dir, const std::string& account_id) {
  RelayInboxCursors cursors;
  if (profile_data_dir.empty() || account_id.empty()) {
    return cursors;
  }
  const auto root = ReadCursorFile(profile_data_dir);
  if (!root || IsNewer(*root)) {
    return cursors;
  }
  auto stored_id = root->getString("account_id");
  if (!stored_id || *stored_id != account_id) {
    return cursors;
  }
  cursors.preferred = root->getString("cursor").value_or("");
  cursors.preferred_relay = root->getString("cursor_relay").value_or("");
  if (const Object* relays = root->getObject("relays")) {
    for (const auto& [origin, value] : relays->entries()) {
      if (auto cursor = asString(value); cursor && !origin.empty() && !cursor->empty()) {
        cursors.others[origin] = *cursor;
      }
    }
  }
  if (const Object* registrations = root->getObject("registrations")) {
    cursors.registrations.emplace();
    for (const auto& [origin, value] : registrations->entries()) {
      if (auto id = asString(value); id && !origin.empty() && !id->empty()) {
        (*cursors.registrations)[origin] = *id;
      }
    }
  }
  if (const Object* floors = root->getObject("reread_floor_ms")) {
    for (const auto& [origin, value] : floors->entries()) {
      if (auto ms = asNonNegInt(value);
          ms && !origin.empty() && *ms > 0 && *ms <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
        cursors.reread_floors_ms[origin] = static_cast<int64_t>(*ms);
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
  if (const auto kept = ReadCursorFile(profile_data_dir); kept && IsNewer(*kept)) {
    return; // a cursor not kept only fetches again what the inbox drops as already seen
  }
  std::error_code ec;
  std::filesystem::create_directories(profile_data_dir, ec);
  Object root;
  root.setJsonUInt("schema_version", kRelayInboxCursorSchemaVersion);
  root.set("account_id", account_id);
  if (!cursors.preferred.empty()) {
    root.set("cursor", cursors.preferred);
    if (!cursors.preferred_relay.empty()) {
      root.set("cursor_relay", cursors.preferred_relay);
    }
  }
  if (!cursors.others.empty()) {
    Object relays;
    for (const auto& [origin, cursor] : cursors.others) {
      relays.set(origin, cursor);
    }
    root.set("relays", std::move(relays));
  }
  // Added beside the cursors (an older app reads the file as before), and always: its presence says the cursors were
  // kept by an app that notes their mailbox.
  Object registrations;
  for (const auto& [origin, id] : cursors.registrations.value_or(std::map<std::string, std::string>{})) {
    registrations.set(origin, id);
  }
  root.set("registrations", std::move(registrations));
  if (!cursors.reread_floors_ms.empty()) { // additive too: an older app ignores it
    Object floors;
    for (const auto& [origin, ms] : cursors.reread_floors_ms) {
      floors.set(origin, ms);
    }
    root.set("reread_floor_ms", std::move(floors));
  }
  // Replaced whole, not synced: written at every sync, and a cursor lost to a crash only fetches again what the
  // inbox drops as already seen.
  (void)AtomicFileWrite::Write(CursorPath(profile_data_dir).string(), DumpJson(root), AtomicFileWrite::Sync::None);
}

std::optional<int64_t> RelayInboxCursorFileWrittenMs(const std::string& profile_data_dir) {
  if (profile_data_dir.empty()) {
    return std::nullopt;
  }
  std::error_code ec;
  const auto written = std::filesystem::last_write_time(CursorPath(profile_data_dir), ec);
  if (ec) {
    return std::nullopt;
  }
  // The file clock's epoch differs by library (and clock_cast / file_clock::to_sys are not everywhere): its age, read
  // on the file clock, taken from the wall clock's now.
  using std::chrono::duration_cast;
  using std::chrono::milliseconds;
  const auto age = duration_cast<milliseconds>(std::filesystem::file_time_type::clock::now() - written);
  const auto now = duration_cast<milliseconds>(std::chrono::system_clock::now().time_since_epoch());
  return (now - age).count();
}

} // namespace pbr
