#pragma once

#include "common/Error.h"
#include "common/PbrCompat.h"

#include <cstdint>
#include <string>

namespace pbr {

/** The device's memory of "Later" on the Chats page's "new version" card (MachinePreferences::update_card_*). */
struct UpdateCardState {
  /** The latest_client_version the count below belongs to. */
  std::string version;
  int dismissals = 0;
  int64_t dismissed_ms = 0;
};

/**
 * Reads and writes that memory in `{data_dir}/machine.json` (RunLogPrefs::MachineDir picks the directory). A file
 * that cannot be read counts as "never tapped"; a save is refused, nothing written, when the file cannot be read (its
 * other settings would be lost).
 */
class UpdatePrefs {
public:
  static UpdateCardState Read(const std::string& data_dir);
  static Roe<void> Write(const std::string& data_dir, const UpdateCardState& state);
};

} // namespace pbr
