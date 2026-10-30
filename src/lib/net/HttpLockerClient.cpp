#include "lib/net/HttpLockerClient.h"

#include "lib/net/HttpClient.h"
#include "lib/net/RelaySignBytes.h"
#include "common/Utilities.h"
#include "common/ValueJson.h"
#include "common/PbrCompat.h"

namespace pbr {

namespace {

constexpr const char kLockerPutDomain[] = "pp-browser:relay-locker-put-v1";
constexpr uint8_t kLockerPutSignVersion = 1;

std::string ErrorText(const HttpResponse& response) {
  if (auto root = TryParseObject(response.body)) {
    if (auto error = root->getString("error")) {
      return *error;
    }
  }
  return "HTTP status " + std::to_string(response.status_code);
}

Object RecordToJson(const LockerRecord& record) {
  Object json;
  json.set("locker_id", record.locker_id);
  json.setJsonUInt("version", record.version);
  json.set("signing_public_key_b64", record.signing_public_key_b64);
  json.set("sealed_b64", record.sealed_b64);
  json.set("signature_b64", record.signature_b64);
  return json;
}

} // namespace

std::vector<uint8_t> BuildLockerPutSignBytes(const std::string& relay_user_id, const std::string& locker_id,
                                             const uint64_t version, const int64_t timestamp) {
  std::ostringstream oss;
  RelaySignAppendDomain(oss, kLockerPutDomain);
  RelaySignAppendU8(oss, kLockerPutSignVersion);
  RelaySignAppendWireLenUtf8(oss, relay_user_id);
  RelaySignAppendWireLenUtf8(oss, locker_id);
  RelaySignAppendU64(oss, version);
  RelaySignAppendI64(oss, timestamp);
  return RelaySignOssToBytes(oss);
}

HttpLockerClient::HttpLockerClient(std::string base_url) : base_url_(std::move(base_url)) {}

Roe<void> HttpLockerClient::Put(const std::string& relay_user_id, const LockerRecord& record,
                                std::optional<LockerPutRefusal>* refused) {
  if (base_url_.empty()) {
    return Error("Locker base_url not configured");
  }
  if (!auth_signer_) {
    return Error("Locker auth signer not configured");
  }
  const int64_t timestamp = util::NowUnixMs();
  auto signature = auth_signer_(BuildLockerPutSignBytes(relay_user_id, record.locker_id, record.version, timestamp));
  if (!signature) {
    return signature.error();
  }
  Object body;
  body.set("relay_user_id", relay_user_id);
  body.set("timestamp", timestamp);
  body.set("signature", *signature);
  body.set("locker", ObjectValue(RecordToJson(record)));
  auto response =
      HttpClient::Post(base_url_ + "/v1/lockers/put", DumpJson(body), {{"Content-Type", "application/json"}});
  if (!response) {
    return response.error();
  }
  if (response->status_code >= 200 && response->status_code < 300) {
    return {};
  }
  if (refused) {
    if (response->status_code == 409) {
      *refused = LockerPutRefusal::NotNewer;
    } else if (response->status_code == 403 && ErrorText(*response) == "Locker quota reached") {
      *refused = LockerPutRefusal::QuotaReached;
    }
  }
  return Error("Locker put refused: " + ErrorText(*response));
}

Roe<std::optional<LockerRecord>> HttpLockerClient::Get(const std::string& locker_id) {
  if (base_url_.empty()) {
    return Error("Locker base_url not configured");
  }
  auto response = HttpClient::Get(base_url_ + "/v1/lockers/" + locker_id);
  if (!response) {
    return response.error();
  }
  if (response->status_code == 404) {
    return std::optional<LockerRecord>{};
  }
  if (response->status_code < 200 || response->status_code >= 300) {
    return Error("Locker get failed: " + ErrorText(*response));
  }
  auto root = TryParseObject(response->body);
  const Object* locker = root ? root->getObject("locker") : nullptr;
  if (!locker) {
    return Error("Locker get: no locker in the answer");
  }
  LockerRecord record;
  record.locker_id = locker->getString("locker_id").value_or("");
  record.version = locker->getNonNegInt("version").value_or(0);
  record.signing_public_key_b64 = locker->getString("signing_public_key_b64").value_or("");
  record.sealed_b64 = locker->getString("sealed_b64").value_or("");
  record.signature_b64 = locker->getString("signature_b64").value_or("");
  if (record.locker_id != locker_id) {
    return Error("Locker get: another locker in the answer");
  }
  return std::optional<LockerRecord>(std::move(record));
}

} // namespace pbr
