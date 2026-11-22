#pragma once

#include "lib/net/RelaySignBytes.h"

#include "common/ExtArea.h"

#include <cstdint>
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
 * The relay API's signed bytes (release-compatibility SIGNED_FORMATS § 2.8): domain `pp-browser:relay-api`, sign
 * version 4, the relay it is for (`audience`, its PeerId), `op`, `timestamp`, the op's fields, the extension area:
 *
 *   "pp-browser:relay-api" 00 ‖ u8 4 ‖ LenUtf8 audience ‖ u8 op ‖ i64 timestamp ‖ …the op's fields… ‖ ext area
 *
 * Messaging's ops: lib/delivery/RelayMessageSignPayload.h; push devices': below. Empty for an empty audience.
 */
std::vector<uint8_t> BuildRelayApiBytes(const std::string& audience, RelayApiOp op, int64_t timestamp,
                                        const pp::ExtArea& ext, const RelayCallFields& fields);

/** `POST /v1/devices/register` (op 3): the device's push token. */
std::vector<uint8_t> BuildRelayApiDeviceRegisterSignBytes(const std::string& audience, const std::string& account_id,
                                                          const std::string& platform, const std::string& device_id,
                                                          const std::string& push_token, int64_t timestamp,
                                                          const pp::ExtArea& ext = {});

/**
 * `POST /v1/devices/unregister` (op 4): the binding it removes, by the id `devices/register` answered (MR007 item 5:
 * a late unregister leaves a newer binding alone).
 */
std::vector<uint8_t> BuildRelayApiDeviceUnregisterSignBytes(const std::string& audience, const std::string& account_id,
                                                            const std::string& platform, const std::string& device_id,
                                                            const std::string& binding_id, int64_t timestamp,
                                                            const pp::ExtArea& ext = {});

} // namespace pbr
