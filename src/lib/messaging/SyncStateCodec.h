#pragma once

#include "common/thread/SyncStateTypes.h"

#include "common/Error.h"

#include <string>
#include "common/PbrCompat.h"

namespace pbr {

std::string PeerSyncPhaseToString(PeerSyncPhase phase);
PeerSyncPhase PeerSyncPhaseFromString(const std::string& value);

std::string PeerSyncStateToJson(const PeerSyncState& state);
Roe<PeerSyncState> PeerSyncStateFromJson(const std::string& json);

} // namespace pbr
