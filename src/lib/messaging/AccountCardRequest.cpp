#include "lib/messaging/AccountCardRequest.h"

#include "common/ValueJson.h"

#include <utility>

namespace pbr {

std::string EncodeAccountCardRequest(const AccountCardRequest& request) {
  Object json;
  json.set("op", "get_card");
  json.set("account_id", request.account_id);
  Object auth;
  auth.set("requester_account_id", request.requester_account_id);
  auth.set("requester_peer_id", request.requester_peer_id);
  auth.set("timestamp_ms", request.timestamp_ms);
  auth.set("signature", request.signature);
  if (request.requester_keys) {
    auth.set("requester_keys", KeyCardToJson(*request.requester_keys));
  }
  if (request.requester_card) {
    auth.set("requester_card", AccountCardToJson(*request.requester_card));
  }
  json.set("auth", auth);
  return DumpJson(json);
}

Roe<AccountCardRequest> DecodeAccountCardRequest(const std::string& json_text) {
  auto json = TryParseObject(json_text);
  if (!json || json->getString("op").value_or("") != "get_card") {
    return Error("account card: not a get_card request");
  }
  AccountCardRequest request;
  request.account_id = json->getString("account_id").value_or("");
  if (request.account_id.empty()) {
    return Error("account card: get_card names no account");
  }
  if (const Object* auth = json->getObject("auth")) {
    request.requester_account_id = auth->getString("requester_account_id").value_or("");
    request.requester_peer_id = auth->getString("requester_peer_id").value_or("");
    request.timestamp_ms = auth->getIf<int64_t>("timestamp_ms").value_or(0);
    request.signature = auth->getString("signature").value_or("");
    if (const Object* keys = auth->getObject("requester_keys")) {
      if (auto card = KeyCardFromJson(*keys)) {
        request.requester_keys = std::move(card.value());
      }
    }
    if (const Object* card_json = auth->getObject("requester_card")) {
      if (auto card = AccountCardFromJson(*card_json)) {
        request.requester_card = std::move(card.value());
      }
    }
  }
  return request;
}

std::string EncodeAccountCardReply(const AccountCardReply& reply) {
  Object json;
  if (reply.card) {
    json.set("card", AccountCardToJson(*reply.card));
  } else if (reply.key_card) {
    json.set("key_card", KeyCardToJson(*reply.key_card));
  } else {
    json.set("error", reply.error.empty() ? std::string("not_available") : reply.error);
  }
  return DumpJson(json);
}

Roe<AccountCardReply> DecodeAccountCardReply(const std::string& json_text) {
  auto json = TryParseObject(json_text);
  if (!json) {
    return Error("account card: reply is not JSON");
  }
  AccountCardReply reply;
  if (const Object* card = json->getObject("card")) {
    auto decoded = AccountCardFromJson(*card);
    if (!decoded) {
      return decoded.error();
    }
    reply.card = std::move(decoded.value());
  } else if (const Object* key_card = json->getObject("key_card")) {
    auto decoded = KeyCardFromJson(*key_card);
    if (!decoded) {
      return decoded.error();
    }
    reply.key_card = std::move(decoded.value());
  } else {
    reply.error = json->getString("error").value_or("not_available");
  }
  return reply;
}

} // namespace pbr
