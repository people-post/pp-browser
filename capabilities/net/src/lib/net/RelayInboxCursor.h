#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>

namespace pbr {

/**
 * Where each of our relay inboxes was polled up to (docs/contracts/DATA_LAYOUT.md `relay_inbox_cursor.json`): the
 * preferred relay's cursor, and the other relays' by origin (multi-relay R2). An inbox's ack is soft, so a cursor lost
 * fetches that inbox again from its oldest message, which the receive pipeline drops as already seen.
 */
struct RelayInboxCursors {
  std::string preferred;
  /** The relay `preferred` is the cursor of (RelayOriginKey); empty: a file from before My relays, the preferred's. */
  std::string preferred_relay;
  /** By relay origin (RelayOriginKey). */
  std::map<std::string, std::string> others;
  /**
   * The mailbox each relay's cursor counts in, by relay origin (RelayOriginKey; the preferred relay's too): the
   * `registration_id` we held there (a relay missing: not known when its cursor was kept). A fresh registration is a
   * new mailbox, numbered from the start again. None at all — a file from before it, or one an older app wrote
   * again — says nothing of any cursor's mailbox. Always written (empty too).
   */
  std::optional<std::map<std::string, std::string>> registrations;

  bool operator==(const RelayInboxCursors&) const = default;
};

/** `relay_inbox_cursor.json`'s schema: a file without one is this one (INVENTORY S3). */
inline constexpr uint64_t kRelayInboxCursorSchemaVersion = 1;

/** The cursors kept for `account_id` (none: empty — another account's file, or a newer app's, counts as none). */
RelayInboxCursors LoadRelayInboxCursors(const std::string& profile_data_dir, const std::string& account_id);
/** Replaces the file whole (frequent writes: not synced); a newer app's file is left as it is. */
void SaveRelayInboxCursors(const std::string& profile_data_dir, const std::string& account_id,
                           const RelayInboxCursors& cursors);

} // namespace pbr
