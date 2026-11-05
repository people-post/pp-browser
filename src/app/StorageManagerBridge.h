#pragma once

#include "feature/conversations/ConversationsFacade.h"
#include "feature/settings/StorageManagerPorts.h"

#include <functional>
#include <string>

namespace pbr {

/**
 * Me → Storage → Manage storage: its ports, filled from the messaging facade. `profile_data_dir` is read when
 * the usage is counted (the active profile's folder).
 */
StorageManagerPorts MakeStorageManagerPorts(ConversationsFacade& facade,
                                            std::function<std::string()> profile_data_dir);

} // namespace pbr
