#pragma once

#include <string>

namespace pbr {

/** Persist relay inbox poll watermark under the profile data dir (frequent writes). */
std::string LoadRelayInboxCursor(const std::string& profile_data_dir, const std::string& relay_user_id);
void SaveRelayInboxCursor(const std::string& profile_data_dir, const std::string& relay_user_id,
                          const std::string& cursor);

} // namespace pbr
