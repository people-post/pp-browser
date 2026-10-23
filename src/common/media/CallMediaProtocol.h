#pragma once

#include "common/PbrCompat.h"

namespace pbr {

/**
 * pp-browser's call-media protocol (domain/call_media), served on the mesh's links and advertised
 * through MeshHost::SetAppProtocols; the mesh itself never names it.
 */
inline constexpr const char* kRealtimeProtocolId = "/pp/realtime/1.0.0";
inline constexpr const char* kCallMediaDirectProtocolId = kRealtimeProtocolId;

} // namespace pbr
