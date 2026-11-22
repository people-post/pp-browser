#include "lib/delivery/RelayMessageSignPayload.h"

#include "common/chat/RelayStreamKey.h"
#include "lib/net/RelayApiSignPayload.h"

namespace pbr {

namespace {

uint8_t HistoryOrderWire(const std::string& order) {
  return order == "desc" ? 1 : 0;
}

/** The inbox readers' layout: the requester, then one string (a cursor, a time). */
std::vector<uint8_t> BuildReaderSignBytes(const std::string& audience, const RelayApiOp op,
                                          const std::string& requester_account_id, const std::string& text,
                                          const int64_t timestamp, const pp::ExtArea& ext) {
  return BuildRelayApiBytes(audience, op, timestamp, ext,
                            [&](SignedBytesWriter& out) { out.Utf8(requester_account_id).Utf8(text); });
}

} // namespace

std::vector<uint8_t> BuildRelayApiSendSignBytes(const std::string& audience, const RelayWireSendRecord& record,
                                                const int64_t timestamp, const pp::ExtArea& ext) {
  return BuildRelayApiBytes(audience, RelayApiOp::Send, timestamp, ext, [&](SignedBytesWriter& out) {
    out.Utf8(record.sender_account_id).Utf8(record.recipient_account_id).Utf8(record.stream_id).U64(record.index_key);
  });
}

std::vector<uint8_t> BuildRelayApiPollInboxSignBytes(const std::string& audience,
                                                     const std::string& requester_account_id, const std::string& cursor,
                                                     const int64_t timestamp, const pp::ExtArea& ext) {
  return BuildReaderSignBytes(audience, RelayApiOp::PollInbox, requester_account_id, cursor, timestamp, ext);
}

std::vector<uint8_t> BuildRelayApiAckInboxSignBytes(const std::string& audience,
                                                    const std::string& requester_account_id, const std::string& cursor,
                                                    const int64_t timestamp, const pp::ExtArea& ext) {
  return BuildReaderSignBytes(audience, RelayApiOp::AckInbox, requester_account_id, cursor, timestamp, ext);
}

std::vector<uint8_t> BuildRelayApiClearInboxSignBytes(const std::string& audience,
                                                      const std::string& requester_account_id,
                                                      const std::string& before_created_at, const int64_t timestamp,
                                                      const pp::ExtArea& ext) {
  return BuildReaderSignBytes(audience, RelayApiOp::ClearInbox, requester_account_id, before_created_at, timestamp,
                              ext);
}

std::vector<uint8_t> BuildRelayApiStreamHistorySignBytes(const std::string& audience, const ChatHistoryRequest& request,
                                                         const int64_t timestamp, const pp::ExtArea& ext) {
  const std::string stream_id = BuildCanonicalRelayStreamKey(
      request.requester_identity_value, request.peer_identity_value, request.channel, request.session_epoch);
  return BuildRelayApiBytes(audience, RelayApiOp::StreamHistory, timestamp, ext, [&](SignedBytesWriter& out) {
    out.Utf8(request.requester_identity_value).Utf8(request.peer_identity_value).Utf8(stream_id);
    uint8_t flags = 0;
    if (request.min_sender_seq) {
      flags |= 0x01;
    }
    if (request.max_sender_seq) {
      flags |= 0x02;
    }
    out.U8(flags);
    if (request.min_sender_seq) {
      out.U64(*request.min_sender_seq);
    }
    if (request.max_sender_seq) {
      out.U64(*request.max_sender_seq);
    }
    out.U64(request.limit).U8(HistoryOrderWire(request.order));
  });
}

} // namespace pbr
