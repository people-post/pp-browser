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
 * The relay API's signed header: domain `pp-browser:relay-api-v1`, version 2 (Account IDs), `op`,
 * `timestamp`. Each call's
 * fields follow it (messaging's: lib/messaging/RelayMessageSignPayload.h; push devices': below).
 */
void AppendRelayApiHeader(std::ostringstream& oss, RelayApiOp op, int64_t timestamp);

std::vector<uint8_t> BuildRelayApiDeviceSignBytes(RelayApiOp op, const std::string& account_id,
                                                  const std::string& platform, const std::string& device_id,
                                                  const std::string& push_token, int64_t timestamp);

} // namespace pbr
