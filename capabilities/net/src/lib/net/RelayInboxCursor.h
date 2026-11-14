#pragma once

#include <map>
#include <string>

namespace pbr {

/**
 * Where each of our relay inboxes was polled up to (docs/contracts/DATA_LAYOUT.md `relay_inbox_cursor.json`): the
 * preferred relay's cursor, and the other relays' by origin (multi-relay R2). An inbox's ack is soft, so a cursor lost
 * fetches that inbox again from its oldest message, which the receive pipeline drops as already seen.
 */
struct RelayInboxCursors {
  std::string preferred;
  /** By relay origin (RelayOriginKey). */
  std::map<std::string, std::string> others;

  bool operator==(const RelayInboxCursors&) const = default;
};

/** The cursors kept for `account_id` (none: empty — another account's file counts as none). */
RelayInboxCursors LoadRelayInboxCursors(const std::string& profile_data_dir, const std::string& account_id);
/** Replaces the file whole (frequent writes: not synced). */
void SaveRelayInboxCursors(const std::string& profile_data_dir, const std::string& account_id,
                           const RelayInboxCursors& cursors);

} // namespace pbr
