#include "lib/delivery/RelayMessageSignPayload.h"

#include "common/chat/RelayStreamKey.h"
#include "lib/net/RelayApiSignPayload.h"
#include "lib/net/RelaySignBytes.h"

namespace pbr {

namespace {

uint8_t HistoryOrderWire(const std::string& order) {
  return order == "desc" ? 1 : 0;
}

} // namespace

std::vector<uint8_t> BuildRelayApiSendSignBytes(const std::string& audience, const RelayWireSendRecord& record,
                                                const int64_t timestamp) {
  std::ostringstream oss;
  if (!AppendRelayApiHeader(oss, audience, RelayApiOp::Send, timestamp)) {
    return {};
  }
  RelaySignAppendWireLenUtf8(oss, record.sender_account_id);
  RelaySignAppendWireLenUtf8(oss, record.recipient_account_id);
  RelaySignAppendWireLenUtf8(oss, record.stream_id);
  RelaySignAppendU64(oss, record.index_key);
  return RelaySignOssToBytes(oss);
}

std::vector<uint8_t> BuildRelayApiPollInboxSignBytes(const std::string& audience,
                                                     const std::string& requester_account_id, const std::string& cursor,
                                                     const int64_t timestamp) {
  std::ostringstream oss;
  if (!AppendRelayApiHeader(oss, audience, RelayApiOp::PollInbox, timestamp)) {
    return {};
  }
  RelaySignAppendWireLenUtf8(oss, requester_account_id);
  RelaySignAppendWireLenUtf8(oss, cursor);
  return RelaySignOssToBytes(oss);
}

std::vector<uint8_t> BuildRelayApiAckInboxSignBytes(const std::string& audience,
                                                    const std::string& requester_account_id, const std::string& cursor,
                                                    const int64_t timestamp) {
  std::ostringstream oss;
  if (!AppendRelayApiHeader(oss, audience, RelayApiOp::AckInbox, timestamp)) {
    return {};
  }
  RelaySignAppendWireLenUtf8(oss, requester_account_id);
  RelaySignAppendWireLenUtf8(oss, cursor);
  return RelaySignOssToBytes(oss);
}

std::vector<uint8_t> BuildRelayApiClearInboxSignBytes(const std::string& audience,
                                                      const std::string& requester_account_id,
                                                      const std::string& before_created_at, const int64_t timestamp) {
  std::ostringstream oss;
  if (!AppendRelayApiHeader(oss, audience, RelayApiOp::ClearInbox, timestamp)) {
    return {};
  }
  RelaySignAppendWireLenUtf8(oss, requester_account_id);
  RelaySignAppendWireLenUtf8(oss, before_created_at);
  return RelaySignOssToBytes(oss);
}

std::vector<uint8_t> BuildRelayApiStreamHistorySignBytes(const std::string& audience, const ChatHistoryRequest& request,
                                                         const int64_t timestamp) {
  const std::string stream_id = BuildCanonicalRelayStreamKey(
      request.requester_identity_value, request.peer_identity_value, request.channel, request.session_epoch);

  std::ostringstream oss;
  if (!AppendRelayApiHeader(oss, audience, RelayApiOp::StreamHistory, timestamp)) {
    return {};
  }
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

} // namespace pbr
