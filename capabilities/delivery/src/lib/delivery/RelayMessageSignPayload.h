#pragma once

// What the relay's message API signs, call by call: the relay API header (lib/net/RelayApiSignPayload.h, sign
// version 2) and the call's fields, every party by Account ID (multi-relay MR001).

#include "common/chat/RelayEnvelope.h"
#include "common/thread/ChatHistoryTypes.h"

#include <cstdint>
#include <string>
#include <vector>

namespace pbr {

std::vector<uint8_t> BuildRelayApiSendSignBytes(const RelayWireSendRecord& record, int64_t timestamp);

std::vector<uint8_t> BuildRelayApiPollInboxSignBytes(const std::string& requester_account_id, const std::string& cursor,
                                                     int64_t timestamp);

std::vector<uint8_t> BuildRelayApiAckInboxSignBytes(const std::string& requester_account_id, const std::string& cursor,
                                                    int64_t timestamp);

std::vector<uint8_t> BuildRelayApiClearInboxSignBytes(const std::string& requester_account_id,
                                                      const std::string& before_created_at, int64_t timestamp);

std::vector<uint8_t> BuildRelayApiStreamHistorySignBytes(const ChatHistoryRequest& request, int64_t timestamp);

} // namespace pbr
