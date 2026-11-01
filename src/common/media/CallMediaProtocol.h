#pragma once

#include "common/PbrCompat.h"
#include "common/ProtocolIds.h"

namespace pbr {

/**
 * pp-browser's call-media protocol (domain/call_media; the wire id kRealtimeProtocolId is
 * common/ProtocolIds.h's), served on the mesh's links and advertised through MeshHost::SetAppProtocols.
 */
inline constexpr const char* kCallMediaDirectProtocolId = kRealtimeProtocolId;

} // namespace pbr
