#pragma once

// What the relay's message API signs, call by call (release-compatibility SIGNED_FORMATS § 2.8): the relay API header
// (lib/net/RelayApiSignPayload.h, sign version 4: the relay the call is for, `audience`, multi-relay MR007, then `op`
// and `timestamp`), the call's fields, every party by Account ID (MR001), and the extension area. Empty for an empty
// audience.

#include "common/chat/RelayEnvelope.h"
#include "common/thread/ChatHistoryTypes.h"

#include "common/ExtArea.h"

#include <cstdint>
#include <string>
#include <vector>

namespace pbr {

std::vector<uint8_t> BuildRelayApiSendSignBytes(const std::string& audience, const RelayWireSendRecord& record,
                                                int64_t timestamp, const pp::ExtArea& ext = {});

std::vector<uint8_t> BuildRelayApiPollInboxSignBytes(const std::string& audience,
                                                     const std::string& requester_account_id, const std::string& cursor,
                                                     int64_t timestamp, const pp::ExtArea& ext = {});

std::vector<uint8_t> BuildRelayApiAckInboxSignBytes(const std::string& audience,
                                                    const std::string& requester_account_id, const std::string& cursor,
                                                    int64_t timestamp, const pp::ExtArea& ext = {});

std::vector<uint8_t> BuildRelayApiClearInboxSignBytes(const std::string& audience,
                                                      const std::string& requester_account_id,
                                                      const std::string& before_created_at, int64_t timestamp,
                                                      const pp::ExtArea& ext = {});

std::vector<uint8_t> BuildRelayApiStreamHistorySignBytes(const std::string& audience, const ChatHistoryRequest& request,
                                                         int64_t timestamp, const pp::ExtArea& ext = {});

} // namespace pbr
