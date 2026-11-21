#include "lib/delivery/HttpRelayClient.h"
#include "lib/delivery/RelayGrantSigner.h"
#include "foundation/tls/RelayClientHeader.h"

#include "common/Diag.h"
#include "common/Utilities.h"
#include "common/ValueJson.h"
#include "common/chat/MessagingJson.h"
#include "foundation/crypto/CryptoUtil.h"
#include "foundation/crypto/SignedBytes.h"
#include "foundation/error/AppError.h"
#include "lib/delivery/RelayMessageSignPayload.h"
#include "lib/people/CardKeys.h"
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

/** A refused call: the audience hears the codes it acts on (MR007); else the status says it, as before. */
Error RelayCallRefusal(IRelayAudience* audience, const ShareWord action, const long status, const std::string& body) {
  if (auto typed = NoteRelayRefusal(audience, status, body)) {
    return *typed;
  }
  return RelayHttpStatusError(action, status, body);
}

/** A send's refusal the client meets itself (its key, its grant) or the caller handles by kind (MR003 / MR005 / MR007). */
Error RelaySendRefusal(IRelayAudience* audience, const long status, const std::string& code, const std::string& body) {
  if (auto typed = NoteRelayRefusal(audience, status, body)) {
    return *typed; // wrong_audience: the sender tries the recipient's next relay
  }
  if (status == 404 && code == "recipient_not_registered") {
    return AppError::Network(Err::Network::RecipientNotRegistered, "Relay send: the recipient is not registered here");
  }
  if (status == 402 && code == "payment_required") {
    int64_t floor = 0;
    if (auto json = TryParseObject(body)) {
      floor = json->getIf<int64_t>("initiation_floor").value_or(0);
    }
    return AppError::Network(Err::Network::PaymentRequired,
                             "Relay send: the recipient charges strangers initiation_floor=" + std::to_string(floor));
  }
  if (status == 429 && code == "stranger_quota_exceeded") {
    return AppError::Auth(Err::Auth::StrangerQuota, "Relay send: the recipient's quota for strangers is spent");
  }
  if (status == 429) {
    return AppError::Auth(Err::Auth::RateLimited, "Relay rate limit exceeded");
  }
  return RelayHttpStatusError("send", status, body);
}

} // namespace

Error RelayPollHttpError(const long status_code, const std::string& body) {
  Error error = RelayHttpStatusError("poll", status_code, body);
  // The relay has no (active) registration for our account: told apart so we register again.
  if (status_code == 403 && RelayRefusalCode(body) == "not_registered") {
    return AppError::Auth(Err::Auth::NotRegistered, error.message);
  }
  return error;
}

bool IsRelayUserNotRegistered(const Error& error) {
  return AppError::CategoryOf(error) == ErrorCategory::Auth &&
         error.code == static_cast<int32_t>(Err::Auth::NotRegistered);
}

Roe<SignedRelayCommand> HttpRelayClient::Sign(const int64_t timestamp, const RelayCommandBytes& build) const {
  return SignRelayCommand(audience_.get(), auth_signer_, timestamp, build);
}

HttpRelayClient::HttpRelayClient(std::string base_url, bool public_only)
    : base_url_(std::move(base_url)), public_only_(public_only) {}

void HttpRelayClient::SetSendHooks(RelaySendHooks hooks) {
  std::lock_guard lock(hooks_mutex_);
  send_hooks_ = std::move(hooks);
}

RelaySendHooks HttpRelayClient::SendHooks() const {
  std::lock_guard lock(hooks_mutex_);
  return send_hooks_;
}

Roe<void> HttpRelayClient::PostSend(RelayWireSendRecord& record, const RelayEnvelope& envelope, SendRetry& retry) {
  retry = SendRetry::None;
  const int64_t timestamp = util::NowUnixMs();
  auto signed_send = Sign(timestamp, [&record, timestamp](const std::string& audience, const pp::ExtArea& ext) {
    return BuildRelayApiSendSignBytes(audience, record, timestamp, ext);
  });
  if (!signed_send) {
    return signed_send.error();
  }
  record.sign_version = signed_send->sign_version;
  record.audience = signed_send->audience;
  record.timestamp = signed_send->timestamp;
  record.signature = signed_send->signature;

  const std::string url = base_url_ + "/v1/messages";
  Object send_body = RelayWireSendRecordToJson(record);
  WriteExtB64(send_body, signed_send->ext); // the area it signed (SIGNED_FORMATS § 2.8)
  if (record.grant) {
    send_body.set("grant", RelayGrantToJson(*record.grant)); // grant v2 (SIGNED_FORMATS § 2.4)
  }
  const auto response =
      HttpClient::Post(url, DumpJson(send_body), WithRelayClientHeader({{"Content-Type", "application/json"}}),
                       kMaxHttpClientBodyBytes, {}, public_only_);
  if (!response) {
    return response.error();
  }
  const long status = response.value().status_code;
  if (status >= 200 && status < 300) {
    return {};
  }
  const std::string& body = response.value().body;
  const std::string code = RelayRefusalCode(body);
  // Which recipient the relay refused (a group fan-out sends one envelope per member).
  logging::getLogger("HttpRelayClient").warning
      << "relay send refused status=" << status << " code=" << code << " recipient=" << record.recipient_account_id
      << " route=" << envelope.route.kind << " grant=" << (record.grant ? "yes" : "no");
  if (status == 401 && code == "sender_key_required" && record.sender_public_key_b64.empty()) {
    retry = SendRetry::WithKey; // the relay does not hold our key (we are not registered there): it hashes to us
  } else if (status == 401 && (code == "grant_expired" || code == "invalid_grant") && record.grant) {
    retry = SendRetry::WithoutGrant;
  }
  return RelaySendRefusal(audience_.get(), status, code, body);
}

Roe<void> HttpRelayClient::Send(const RelayEnvelope& envelope) {
  if (base_url_.empty()) {
    return AppError::Config(Err::Config::MissingKey, "Relay base_url not configured");
  }
  auto record = RelayWireSendRecordFromEnvelope(envelope);
  if (!record) {
    return record.error();
  }
  const RelaySendHooks hooks = SendHooks();
  // A contact's send carries the recipient's grant (MR005); a send to our own mailbox needs none.
  if (record->recipient_account_id != record->sender_account_id && hooks.grant_for) {
    record->grant = hooks.grant_for(record->recipient_account_id);
  }
  // At most one change of each kind: our key once, the grant dropped once.
  for (int attempt = 0; attempt < 3; ++attempt) {
    SendRetry retry = SendRetry::None;
    auto sent = PostSend(*record, envelope, retry);
    if (sent || retry == SendRetry::None) {
      return sent;
    }
    if (retry == SendRetry::WithKey) {
      const std::string held = hooks.sender_public_key ? hooks.sender_public_key() : std::string();
      auto typed = held.empty() ? Roe<std::string>(Error("no account key"))
                                : TypedKeyB64(pp::BlobKind::kSignaturePublicKey, held);
      if (!typed) {
        return sent;
      }
      record->sender_public_key_b64 = std::move(*typed);
    } else {
      // A grant the relay does not take: ask for a new one, and send this once as a stranger meanwhile.
      if (hooks.grant_refused) {
        hooks.grant_refused(record->recipient_account_id);
      }
      record->grant.reset();
    }
  }
  return Error("Relay send: refused after its retries");
}

Roe<RelayPollResult> HttpRelayClient::PollInbox(const std::string& requester_account_id, const std::string& cursor) {
  if (base_url_.empty()) {
    return Error("Relay base_url not configured");
  }
  const int64_t timestamp = util::NowUnixMs();
  auto signed_poll = Sign(timestamp, [&](const std::string& audience, const pp::ExtArea& ext) {
    return BuildRelayApiPollInboxSignBytes(audience, requester_account_id, cursor, timestamp, ext);
  });
  if (!signed_poll) {
    return signed_poll.error();
  }

  Object body;
  body.set("requester_account_id", requester_account_id);
  body.set("cursor", cursor);
  PutSignedRelayCommand(body, *signed_poll);
  const std::string url = base_url_ + "/v1/inbox/poll";
  // B30: poll-specific fail-fast (serialized polls on cellular; global 30s starved Accept/MediaKey).
  // Other relay HTTP keeps default HttpTimeout{30}. Not an env knob.
  constexpr HttpTimeout kRelayPollTimeout{.total_s = 10, .connect_s = 5};
  const auto response =
      HttpClient::Post(url, DumpJson(body), WithRelayClientHeader({{"Content-Type", "application/json"}}),
                       kMaxHttpClientBodyBytes, kRelayPollTimeout, public_only_);
  if (!response) {
    return response.error();
  }
  if (response.value().status_code < 200 || response.value().status_code >= 300) {
    if (auto typed = NoteRelayRefusal(audience_.get(), response.value().status_code, response.value().body)) {
      return *typed; // wrong_audience: the pin is checked again (MR007)
    }
    return RelayPollHttpError(response.value().status_code, response.value().body);
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
          logging::getLogger("HttpRelayClient").warning << "inbox record skipped sender=" << inbound->sender_account_id
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

Roe<RelayDeleteResult> HttpRelayClient::AckInbox(const std::string& requester_account_id, const std::string& cursor) {
  if (base_url_.empty()) {
    return Error("Relay base_url not configured");
  }
  if (cursor.empty()) {
    return Error("Missing inbox cursor");
  }
  const int64_t timestamp = util::NowUnixMs();
  auto signed_ack = Sign(timestamp, [&](const std::string& audience, const pp::ExtArea& ext) {
    return BuildRelayApiAckInboxSignBytes(audience, requester_account_id, cursor, timestamp, ext);
  });
  if (!signed_ack) {
    return signed_ack.error();
  }

  Object body;
  body.set("requester_account_id", requester_account_id);
  body.set("cursor", cursor);
  PutSignedRelayCommand(body, *signed_ack);
  const std::string url = base_url_ + "/v1/inbox/ack";
  const auto response =
      HttpClient::Post(url, DumpJson(body), WithRelayClientHeader({{"Content-Type", "application/json"}}),
                       kMaxHttpClientBodyBytes, {}, public_only_);
  if (!response) {
    return response.error();
  }
  if (response.value().status_code < 200 || response.value().status_code >= 300) {
    return RelayCallRefusal(audience_.get(), "ack", response.value().status_code, response.value().body);
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

Roe<RelayDeleteResult> HttpRelayClient::ClearInbox(const std::string& requester_account_id,
                                                   const std::string& before_created_at) {
  if (base_url_.empty()) {
    return Error("Relay base_url not configured");
  }
  if (before_created_at.empty()) {
    return Error("Missing before_created_at");
  }
  const int64_t timestamp = util::NowUnixMs();
  auto signed_clear = Sign(timestamp, [&](const std::string& audience, const pp::ExtArea& ext) {
    return BuildRelayApiClearInboxSignBytes(audience, requester_account_id, before_created_at, timestamp, ext);
  });
  if (!signed_clear) {
    return signed_clear.error();
  }

  Object body;
  body.set("requester_account_id", requester_account_id);
  body.set("before_created_at", before_created_at);
  PutSignedRelayCommand(body, *signed_clear);
  const std::string url = base_url_ + "/v1/inbox/clear";
  const auto response =
      HttpClient::Post(url, DumpJson(body), WithRelayClientHeader({{"Content-Type", "application/json"}}),
                       kMaxHttpClientBodyBytes, {}, public_only_);
  if (!response) {
    return response.error();
  }
  if (response.value().status_code < 200 || response.value().status_code >= 300) {
    return RelayCallRefusal(audience_.get(), "clear", response.value().status_code, response.value().body);
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
  auto signed_query = Sign(timestamp, [&](const std::string& audience, const pp::ExtArea& ext) {
    return BuildRelayApiStreamHistorySignBytes(audience, request, timestamp, ext);
  });
  if (!signed_query) {
    return signed_query.error();
  }

  Object body = ChatHistoryRequestToStreamHistoryJson(request);
  PutSignedRelayCommand(body, *signed_query);

  const std::string url = base_url_ + "/v1/streams/messages/query";
  const auto response =
      HttpClient::Post(url, DumpJson(body), WithRelayClientHeader({{"Content-Type", "application/json"}}),
                       kMaxHttpClientBodyBytes, {}, public_only_);
  if (!response) {
    return response.error();
  }
  if (response.value().status_code < 200 || response.value().status_code >= 300) {
    return RelayCallRefusal(audience_.get(), "history", response.value().status_code, response.value().body);
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
