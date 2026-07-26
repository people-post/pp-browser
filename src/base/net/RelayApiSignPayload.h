#pragma once

#include "base/messaging/ThreadTypes.h"

#include <cstdint>
#include <optional>
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

std::vector<uint8_t> BuildRelayApiSendSignBytes(const RelayWireSendRecord& record, int64_t timestamp);

std::vector<uint8_t> BuildRelayApiPollInboxSignBytes(const std::string& requester_contact_id,
                                                     const std::string& cursor, int64_t timestamp);

std::vector<uint8_t> BuildRelayApiAckInboxSignBytes(const std::string& requester_contact_id,
                                                    const std::string& cursor, int64_t timestamp);

std::vector<uint8_t> BuildRelayApiClearInboxSignBytes(const std::string& requester_contact_id,
                                                      const std::string& before_created_at, int64_t timestamp);

std::vector<uint8_t> BuildRelayApiStreamHistorySignBytes(const ChatHistoryRequest& request, int64_t timestamp);

std::vector<uint8_t> BuildRelayApiDeviceSignBytes(RelayApiOp op, const std::string& relay_user_id,
                                                  const std::string& platform, const std::string& device_id,
                                                  const std::string& push_token, int64_t timestamp);

} // namespace pbr
