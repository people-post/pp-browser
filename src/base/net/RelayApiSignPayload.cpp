#include "base/net/RelayApiSignPayload.h"

#include "common/chat/RelayStreamKey.h"
#include "base/net/RelaySignBytes.h"

namespace pbr {

namespace {

constexpr const char kRelayApiDomain[] = "pp-browser:relay-api-v1";
constexpr uint8_t kSignVersion = 1;

void AppendRelayApiHeader(std::ostringstream& oss, RelayApiOp op, const int64_t timestamp) {
  RelaySignAppendDomain(oss, kRelayApiDomain);
  RelaySignAppendU8(oss, kSignVersion);
  RelaySignAppendU8(oss, static_cast<uint8_t>(op));
  RelaySignAppendI64(oss, timestamp);
}

uint8_t HistoryOrderWire(const std::string& order) {
  return order == "desc" ? 1 : 0;
}

} // namespace

std::vector<uint8_t> BuildRelayApiSendSignBytes(const RelayWireSendRecord& record, const int64_t timestamp) {
  std::ostringstream oss;
  AppendRelayApiHeader(oss, RelayApiOp::Send, timestamp);
  RelaySignAppendWireLenUtf8(oss, record.sender_contact_id);
  RelaySignAppendWireLenUtf8(oss, record.recipient_contact_id);
  RelaySignAppendWireLenUtf8(oss, record.stream_id);
  RelaySignAppendU64(oss, record.index_key);
  return RelaySignOssToBytes(oss);
}

std::vector<uint8_t> BuildRelayApiPollInboxSignBytes(const std::string& requester_contact_id,
                                                     const std::string& cursor, const int64_t timestamp) {
  std::ostringstream oss;
  AppendRelayApiHeader(oss, RelayApiOp::PollInbox, timestamp);
  RelaySignAppendWireLenUtf8(oss, requester_contact_id);
  RelaySignAppendWireLenUtf8(oss, cursor);
  return RelaySignOssToBytes(oss);
}

std::vector<uint8_t> BuildRelayApiAckInboxSignBytes(const std::string& requester_contact_id,
                                                    const std::string& cursor, const int64_t timestamp) {
  std::ostringstream oss;
  AppendRelayApiHeader(oss, RelayApiOp::AckInbox, timestamp);
  RelaySignAppendWireLenUtf8(oss, requester_contact_id);
  RelaySignAppendWireLenUtf8(oss, cursor);
  return RelaySignOssToBytes(oss);
}

std::vector<uint8_t> BuildRelayApiClearInboxSignBytes(const std::string& requester_contact_id,
                                                      const std::string& before_created_at,
                                                      const int64_t timestamp) {
  std::ostringstream oss;
  AppendRelayApiHeader(oss, RelayApiOp::ClearInbox, timestamp);
  RelaySignAppendWireLenUtf8(oss, requester_contact_id);
  RelaySignAppendWireLenUtf8(oss, before_created_at);
  return RelaySignOssToBytes(oss);
}

std::vector<uint8_t> BuildRelayApiStreamHistorySignBytes(const ChatHistoryRequest& request,
                                                         const int64_t timestamp) {
  const std::string stream_id =
      BuildCanonicalRelayStreamKey(request.requester_identity_value, request.peer_identity_value, request.channel,
                                 request.session_epoch);

  std::ostringstream oss;
  AppendRelayApiHeader(oss, RelayApiOp::StreamHistory, timestamp);
  RelaySignAppendWireLenUtf8(oss, request.requester_identity_value);
  RelaySignAppendWireLenUtf8(oss, request.peer_identity_value);
  RelaySignAppendWireLenUtf8(oss, stream_id);

  uint8_t flags = 0;
  if (request.min_sender_seq) {
    flags |= 0x01;
  }
  if (request.max_sender_seq) {
    flags |= 0x02;
  }
  RelaySignAppendU8(oss, flags);
  if (request.min_sender_seq) {
    RelaySignAppendU64(oss, *request.min_sender_seq);
  }
  if (request.max_sender_seq) {
    RelaySignAppendU64(oss, *request.max_sender_seq);
  }
  RelaySignAppendU64(oss, request.limit);
  RelaySignAppendU8(oss, HistoryOrderWire(request.order));
  return RelaySignOssToBytes(oss);
}

std::vector<uint8_t> BuildRelayApiDeviceSignBytes(const RelayApiOp op, const std::string& relay_user_id,
                                                  const std::string& platform, const std::string& device_id,
                                                  const std::string& push_token, const int64_t timestamp) {
  std::ostringstream oss;
  AppendRelayApiHeader(oss, op, timestamp);
  RelaySignAppendWireLenUtf8(oss, relay_user_id);
  RelaySignAppendWireLenUtf8(oss, platform);
  RelaySignAppendWireLenUtf8(oss, device_id);
  RelaySignAppendWireLenUtf8(oss, push_token);
  return RelaySignOssToBytes(oss);
}

} // namespace pbr
