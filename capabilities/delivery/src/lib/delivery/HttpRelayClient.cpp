#include "lib/delivery/HttpRelayClient.h"

#include "common/Diag.h"
#include "common/Utilities.h"
#include "common/ValueJson.h"
#include "common/chat/MessagingJson.h"
#include "foundation/crypto/CryptoUtil.h"
#include "foundation/error/AppError.h"
#include "lib/delivery/RelayMessageSignPayload.h"
#include "lib/net/HttpClient.h"

#include "common/Logger.h"
#include "common/PbrCompat.h"
#include "common/MeshAlias.h"

namespace pbr {

namespace {

std::string TruncateForLog(const std::string& text, const size_t max_chars) {
  if (text.size() <= max_chars) {
    return text;
  }
  return text.substr(0, max_chars) + "...";
}

Error RelayHttpStatusError(const ShareWord action, long status_code, const std::string& body) {
  DiagLine("relay.http_failed").Add("action", action).Add("status", status_code).Emit();
  std::string detail = std::string("Relay ") + action.c_str() + " failed with status " + std::to_string(status_code);
  std::string api_message;
  if (auto json = TryParseObject(body)) {
    if (auto err = json->getString("error")) {
      api_message = *err;
    } else if (auto message = json->getString("message")) {
      api_message = *message;
    }
  }
  if (!api_message.empty()) {
    detail += ": " + api_message;
  } else if (!body.empty()) {
    detail += " body=" + TruncateForLog(body, 200);
  }
  logging::getLogger("HttpRelayClient").warning << detail;
  // A 4xx (but a timeout or a rate limit) is the relay refusing this request: final, not a hiccup.
  const bool refused = status_code >= 400 && status_code < 500 && status_code != 408 && status_code != 429;
  return AppError::Network(refused ? Err::Network::HttpRejected : Err::Network::HttpError, detail);
}

} // namespace

Roe<std::string> HttpRelayClient::SignRelayApiBytes(const std::vector<uint8_t>& sign_bytes) const {
  if (!auth_signer_) {
    return Error("Relay auth signer not configured");
  }
  if (sign_bytes.empty()) {
    return Error("Empty relay API sign bytes");
  }
  return auth_signer_(sign_bytes);
}

HttpRelayClient::HttpRelayClient(std::string base_url) : base_url_(std::move(base_url)) {}

Roe<void> HttpRelayClient::Send(const RelayEnvelope& envelope) {
  if (base_url_.empty()) {
    return AppError::Config(Err::Config::MissingKey, "Relay base_url not configured");
  }
  auto record = RelayWireSendRecordFromEnvelope(envelope);
  if (!record) {
    return record.error();
  }
  record->timestamp = util::NowUnixMs();
  const auto sign_bytes = BuildRelayApiSendSignBytes(*record, record->timestamp);
  if (sign_bytes.empty()) {
    return Error("Failed to build relay send sign bytes");
  }
  auto signature = SignRelayApiBytes(sign_bytes);
  if (!signature) {
    return signature.error();
  }
  record->signature = *signature;

  const std::string url = base_url_ + "/v1/messages";
  const auto response =
      HttpClient::Post(url, DumpJson(RelayWireSendRecordToJson(*record)), {{"Content-Type", "application/json"}});
  if (!response) {
    return response.error();
  }
  if (response.value().status_code == 429) {
    return AppError::Auth(Err::Auth::RateLimited, "Relay rate limit exceeded");
  }
  if (response.value().status_code < 200 || response.value().status_code >= 300) {
    // Which recipient the relay refused (a group fan-out sends one envelope per member).
    logging::getLogger("HttpRelayClient").warning << "relay send rejected status=" << response.value().status_code
                                                  << " recipient=" << record->recipient_contact_id
                                                  << " route=" << envelope.route.kind
                                                  << " body=" << response.value().body.substr(0, 160);
    return RelayHttpStatusError("send", response.value().status_code, response.value().body);
  }
  return {};
}

Roe<RelayPollResult> HttpRelayClient::PollInbox(const std::string& requester_contact_id, const std::string& cursor) {
  if (base_url_.empty()) {
    return Error("Relay base_url not configured");
  }
  const int64_t timestamp = util::NowUnixMs();
  const auto sign_bytes = BuildRelayApiPollInboxSignBytes(requester_contact_id, cursor, timestamp);
  if (sign_bytes.empty()) {
    return Error("Failed to build relay poll sign bytes");
  }
  auto signature = SignRelayApiBytes(sign_bytes);
  if (!signature) {
    return signature.error();
  }

  Object body;
  body.set("requester_contact_id", requester_contact_id);
  body.set("cursor", cursor);
  body.set("timestamp", timestamp);
  body.set("signature", *signature);
  const std::string url = base_url_ + "/v1/inbox/poll";
  // B30: poll-specific fail-fast (serialized polls on cellular; global 30s starved Accept/MediaKey).
  // Other relay HTTP keeps default HttpTimeout{30}. Not an env knob.
  constexpr HttpTimeout kRelayPollTimeout{.total_s = 10, .connect_s = 5};
  const auto response = HttpClient::Post(url, DumpJson(body), {{"Content-Type", "application/json"}},
                                         kMaxHttpClientBodyBytes, kRelayPollTimeout);
  if (!response) {
    return response.error();
  }
  if (response.value().status_code < 200 || response.value().status_code >= 300) {
    return RelayHttpStatusError("poll", response.value().status_code, response.value().body);
  }

  auto root = TryParseObject(response.value().body);
  if (!root) {
    return Error("Invalid relay inbox JSON");
  }

  RelayPollResult result;
  if (auto next_cursor = root->getString("next_cursor")) {
    result.next_cursor = *next_cursor;
  }
  if (auto server_time = root->getIf<int64_t>("server_time")) {
    result.server_time_ms = *server_time;
  }
  if (const Array* messages = root->getArray("messages")) {
    for (const Value& item_value : messages->elements) {
      const Object* item = asObject(item_value);
      if (!item) {
        continue;
      }
      if (item->contains("blob_b64")) {
        auto inbound = ParseRelayInboundRecord(*item);
        if (!inbound) {
          logging::getLogger("HttpRelayClient").warning << "inbox record skipped: " << inbound.error().message;
          continue;
        }
        auto envelope = RelayEnvelopeFromInboundRecord(*inbound);
        if (!envelope) {
          // The cursor moves past it: an envelope we cannot read is never seen again.
          logging::getLogger("HttpRelayClient").warning << "inbox record skipped sender=" << inbound->sender_contact_id
                                                        << " stream=" << inbound->stream_id << ": "
                                                        << envelope.error().message;
          continue;
        }
        if (result.server_time_ms) {
          envelope->relay_server_time_ms = result.server_time_ms;
        }
        result.messages.push_back(*envelope);
      } else {
        auto envelope = ParseRelayEnvelope(*item);
        if (envelope) {
          if (result.server_time_ms) {
            envelope->relay_server_time_ms = result.server_time_ms;
          }
          result.messages.push_back(*envelope);
        }
      }
    }
  }
  return result;
}

Roe<RelayDeleteResult> HttpRelayClient::AckInbox(const std::string& requester_contact_id, const std::string& cursor) {
  if (base_url_.empty()) {
    return Error("Relay base_url not configured");
  }
  if (cursor.empty()) {
    return Error("Missing inbox cursor");
  }
  const int64_t timestamp = util::NowUnixMs();
  const auto sign_bytes = BuildRelayApiAckInboxSignBytes(requester_contact_id, cursor, timestamp);
  if (sign_bytes.empty()) {
    return Error("Failed to build relay ack sign bytes");
  }
  auto signature = SignRelayApiBytes(sign_bytes);
  if (!signature) {
    return signature.error();
  }

  Object body;
  body.set("requester_contact_id", requester_contact_id);
  body.set("cursor", cursor);
  body.set("timestamp", timestamp);
  body.set("signature", *signature);
  const std::string url = base_url_ + "/v1/inbox/ack";
  const auto response = HttpClient::Post(url, DumpJson(body), {{"Content-Type", "application/json"}});
  if (!response) {
    return response.error();
  }
  if (response.value().status_code < 200 || response.value().status_code >= 300) {
    return Error("Relay ack failed with status " + std::to_string(response.value().status_code));
  }

  auto root = TryParseObject(response.value().body);
  if (!root) {
    return Error("Invalid relay ack JSON");
  }
  RelayDeleteResult result;
  if (auto deleted = root->getIf<int64_t>("deleted")) {
    result.deleted = *deleted;
  }
  return result;
}

Roe<RelayDeleteResult> HttpRelayClient::ClearInbox(const std::string& requester_contact_id,
                                                   const std::string& before_created_at) {
  if (base_url_.empty()) {
    return Error("Relay base_url not configured");
  }
  if (before_created_at.empty()) {
    return Error("Missing before_created_at");
  }
  const int64_t timestamp = util::NowUnixMs();
  const auto sign_bytes = BuildRelayApiClearInboxSignBytes(requester_contact_id, before_created_at, timestamp);
  if (sign_bytes.empty()) {
    return Error("Failed to build relay clear sign bytes");
  }
  auto signature = SignRelayApiBytes(sign_bytes);
  if (!signature) {
    return signature.error();
  }

  Object body;
  body.set("requester_contact_id", requester_contact_id);
  body.set("before_created_at", before_created_at);
  body.set("timestamp", timestamp);
  body.set("signature", *signature);
  const std::string url = base_url_ + "/v1/inbox/clear";
  const auto response = HttpClient::Post(url, DumpJson(body), {{"Content-Type", "application/json"}});
  if (!response) {
    return response.error();
  }
  if (response.value().status_code < 200 || response.value().status_code >= 300) {
    return Error("Relay clear failed with status " + std::to_string(response.value().status_code));
  }

  auto root = TryParseObject(response.value().body);
  if (!root) {
    return Error("Invalid relay clear JSON");
  }
  RelayDeleteResult result;
  if (auto deleted = root->getIf<int64_t>("deleted")) {
    result.deleted = *deleted;
  }
  return result;
}

Roe<ChatHistoryResponse> HttpRelayClient::FetchChatHistory(const ChatHistoryRequest& request) {
  if (base_url_.empty()) {
    return Error("Relay base_url not configured");
  }
  const int64_t timestamp = util::NowUnixMs();
  const auto sign_bytes = BuildRelayApiStreamHistorySignBytes(request, timestamp);
  if (sign_bytes.empty()) {
    return Error("Failed to build relay history sign bytes");
  }
  auto signature = SignRelayApiBytes(sign_bytes);
  if (!signature) {
    return signature.error();
  }

  Object body = ChatHistoryRequestToStreamHistoryJson(request);
  body.set("timestamp", timestamp);
  body.set("signature", *signature);

  const std::string url = base_url_ + "/v1/streams/messages/query";
  const auto response = HttpClient::Post(url, DumpJson(body), {{"Content-Type", "application/json"}});
  if (!response) {
    return response.error();
  }
  if (response.value().status_code < 200 || response.value().status_code >= 300) {
    return Error("Relay history fetch failed with status " + std::to_string(response.value().status_code));
  }

  auto root = TryParseObject(response.value().body);
  if (!root) {
    return Error("Invalid relay history JSON");
  }
  auto parsed = ChatHistoryResponseFromJson(*root);
  if (!parsed) {
    return parsed.error();
  }
  if (parsed->peer_identity_kind.empty()) {
    parsed->peer_identity_kind = request.peer_identity_kind;
    parsed->peer_identity_value = request.peer_identity_value;
    parsed->channel = request.channel;
    parsed->session_epoch = request.session_epoch;
  }
  return *parsed;
}

} // namespace pbr
