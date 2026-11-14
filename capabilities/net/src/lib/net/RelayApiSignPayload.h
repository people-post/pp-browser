#pragma once

#include <cstdint>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace pbr {

enum class RelayApiOp : uint8_t {
  Send = 0,
  PollInbox = 1,
  StreamHistory = 2,
  DeviceRegister = 3,
  DeviceUnregister = 4,
  AckInbox = 5,
  ClearInbox = 6,
};

/**
 * The relay API's signed header: domain `pp-browser:relay-api-v1`, version 3, the relay it is for (`audience`, its
 * PeerId, multi-relay MR007), `op`, `timestamp`. Each call's fields follow it (messaging's:
 * lib/delivery/RelayMessageSignPayload.h; push devices': below). False, nothing written, for an empty audience.
 */
bool AppendRelayApiHeader(std::ostringstream& oss, const std::string& audience, RelayApiOp op, int64_t timestamp);

/** `POST /v1/devices/register` (op 3): the device's push token. */
std::vector<uint8_t> BuildRelayApiDeviceRegisterSignBytes(const std::string& audience, const std::string& account_id,
                                                          const std::string& platform, const std::string& device_id,
                                                          const std::string& push_token, int64_t timestamp);

/**
 * `POST /v1/devices/unregister` (op 4): the binding it removes, by the id `devices/register` answered (MR007 item 5:
 * a late unregister leaves a newer binding alone).
 */
std::vector<uint8_t> BuildRelayApiDeviceUnregisterSignBytes(const std::string& audience, const std::string& account_id,
                                                            const std::string& platform, const std::string& device_id,
                                                            const std::string& binding_id, int64_t timestamp);

} // namespace pbr
