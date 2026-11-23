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
#include <ratio>
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

/** `reread_phase` values (RelayInboxReread). */
constexpr const char* kRereadDrain = "drain";
constexpr const char* kRereadBackfill = "backfill";

/** `out` = a - b; false (out untouched) when that overflows. */
bool CheckedSub(const int64_t a, const int64_t b, int64_t& out) {
  if ((b > 0 && a < std::numeric_limits<int64_t>::min() + b) ||
      (b < 0 && a > std::numeric_limits<int64_t>::max() + b)) {
    return false;
  }
  out = a - b;
  return true;
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
  std::map<std::string, int64_t> floors;
  if (const Object* kept = root->getObject("reread_floor_ms")) {
    for (const auto& [origin, value] : kept->entries()) {
      if (auto ms = asNonNegInt(value);
          ms && *ms > 0 && *ms <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
        floors[origin] = static_cast<int64_t>(*ms);
      }
    }
  }
  if (const Object* phases = root->getObject("reread_phase")) {
    for (const auto& [origin, value] : phases->entries()) {
      const auto phase = asString(value);
      if (origin.empty() || !phase || (*phase != kRereadDrain && *phase != kRereadBackfill)) {
        continue; // not one this app knows: no re-read there (a floor alone is none either)
      }
      RelayInboxReread reread{.backfill = *phase == kRereadBackfill};
      if (const auto floor = floors.find(origin); floor != floors.end()) {
        reread.floor_ms = floor->second;
      }
      cursors.rereads[origin] = reread;
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
  if (!cursors.rereads.empty()) { // additive too: an older app ignores both
    Object phases;
    Object floors;
    for (const auto& [origin, reread] : cursors.rereads) {
      phases.set(origin, reread.backfill ? kRereadBackfill : kRereadDrain);
      if (reread.floor_ms) {
        floors.set(origin, *reread.floor_ms);
      }
    }
    root.set("reread_phase", std::move(phases));
    if (!floors.empty()) {
      root.set("reread_floor_ms", std::move(floors));
    }
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
  // The file clock's epoch and representation differ by library (libc++: 128-bit ns; MSVC: 100 ns ticks from 1601), and
  // clock_cast / file_clock::to_sys are not everywhere: the file's age, read on the file clock, taken from the wall
  // clock's now. Both file times go to ms first (a division: no overflow), and each subtraction is checked — an
  // extreme time (a copied or corrupt one) is none, never undefined arithmetic.
  using std::chrono::duration_cast;
  using std::chrono::milliseconds;
  using FileTime = std::filesystem::file_time_type;
  static_assert(std::ratio_less_equal_v<FileTime::period, std::milli>, "a file time to ms must be a division");
  const int64_t written_ms = duration_cast<milliseconds>(written.time_since_epoch()).count();
  const int64_t file_now_ms = duration_cast<milliseconds>(FileTime::clock::now().time_since_epoch()).count();
  const int64_t wall_now_ms = duration_cast<milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
  int64_t age_ms = 0;
  int64_t at_ms = 0;
  if (!CheckedSub(file_now_ms, written_ms, age_ms) || !CheckedSub(wall_now_ms, age_ms, at_ms)) {
    return std::nullopt;
  }
  return at_ms;
}

} // namespace pbr
