#pragma once

namespace pbr {

/**
 * Microphone endpoints open in this process (every MediaDeviceArbiter's separate mics, counted from the open until
 * the OS stream is closed). AudioSession reads it: on Android a route change under an open recording stream loses
 * the mic for the process, so the route only changes from there when none is open.
 */
bool AnyMicEndpointOpen();

/** Counts one open mic endpoint for its lifetime (MediaDeviceArbiter wraps each separate mic in one). */
class MicEndpointCount {
public:
  MicEndpointCount();
  ~MicEndpointCount();
  MicEndpointCount(const MicEndpointCount&) = delete;
  MicEndpointCount& operator=(const MicEndpointCount&) = delete;
};

} // namespace pbr
