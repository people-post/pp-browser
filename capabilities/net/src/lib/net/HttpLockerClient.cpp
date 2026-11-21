#include "lib/net/HttpLockerClient.h"
#include "foundation/tls/RelayClientHeader.h"

#include "lib/net/HttpClient.h"
#include "lib/net/RelaySignBytes.h"
#include "common/Utilities.h"
#include "common/ValueJson.h"
#include "common/PbrCompat.h"

namespace pbr {

namespace {

constexpr const char kLockerPutDomain[] = "pp-browser:relay-locker-put-v1";
// 2 (multi-relay MR007): the audience after the version.
constexpr uint8_t kLockerPutSignVersion = 2;

std::string ErrorText(const HttpResponse& response) {
  if (auto root = TryParseObject(response.body)) {
    if (auto error = root->getString("error")) {
      return *error;
    }
  }
  return "HTTP status " + std::to_string(response.status_code);
}

} // namespace

Object LockerRecordToJson(const LockerRecord& record) {
  Object json;
  if (record.record_version != 0) {
    json.set("record_version", record.record_version);
  }
  json.set("locker_id", record.locker_id);
  json.setJsonUInt("version", record.version);
  json.set("signing_public_key_b64", record.signing_public_key_b64);
  json.set("sealed_b64", record.sealed_b64);
  if (!record.ext_b64.empty()) {
    json.set("ext_b64", record.ext_b64);
  }
  json.set("signature_b64", record.signature_b64);
  return json;
}

LockerRecord LockerRecordFromJson(const Object& json) {
  LockerRecord record;
  record.record_version = json.getIf<int64_t>("record_version").value_or(0);
  record.locker_id = json.getString("locker_id").value_or("");
  record.version = json.getNonNegInt("version").value_or(0);
  record.signing_public_key_b64 = json.getString("signing_public_key_b64").value_or("");
  record.sealed_b64 = json.getString("sealed_b64").value_or("");
  record.ext_b64 = json.getString("ext_b64").value_or("");
  record.signature_b64 = json.getString("signature_b64").value_or("");
  return record;
}

std::vector<uint8_t> BuildLockerPutSignBytes(const std::string& audience, const std::string& account_id,
                                             const std::string& locker_id, const uint64_t version,
                                             const int64_t timestamp) {
  std::ostringstream oss;
  if (!RelaySignAppendCommandHeader(oss, kLockerPutDomain, kLockerPutSignVersion, audience)) {
    return {};
  }
  RelaySignAppendWireLenUtf8(oss, account_id);
  RelaySignAppendWireLenUtf8(oss, locker_id);
  RelaySignAppendU64(oss, version);
  RelaySignAppendI64(oss, timestamp);
  return RelaySignOssToBytes(oss);
}

HttpLockerClient::HttpLockerClient(std::string base_url) : base_url_(std::move(base_url)) {}

Roe<void> HttpLockerClient::Put(const std::string& account_id, const LockerRecord& record,
                                std::optional<LockerPutRefused>* refused) {
  if (base_url_.empty()) {
    return Error("Locker base_url not configured");
  }
  const int64_t timestamp = util::NowUnixMs();
  auto signed_put = SignRelayCommand(audience_.get(), auth_signer_, timestamp, [&](const std::string& audience) {
    return BuildLockerPutSignBytes(audience, account_id, record.locker_id, record.version, timestamp);
  });
  if (!signed_put) {
    return signed_put.error();
  }
  Object body;
  body.set("account_id", account_id);
  PutSignedRelayCommand(body, *signed_put);
  body.set("locker", ObjectValue(LockerRecordToJson(record)));
  auto response = HttpClient::Post(base_url_ + "/v1/lockers/put", DumpJson(body),
                                   WithRelayClientHeader({{"Content-Type", "application/json"}}));
  if (!response) {
    return response.error();
  }
  if (response->status_code >= 200 && response->status_code < 300) {
    return {};
  }
  if (auto typed = NoteRelayRefusal(audience_.get(), response->status_code, response->body)) {
    return *typed;
  }
  if (refused) {
    if (response->status_code == 409) {
      // The relay names the version it holds (SPECS § lockers/put): the next write goes above it.
      const auto stored = TryParseObject(response->body);
      const auto version = stored ? stored->getNonNegInt("version") : std::nullopt;
      *refused = LockerPutRefused{.reason = LockerPutRefusal::NotNewer,
                                  .stored_version = version ? static_cast<uint64_t>(*version) : 0};
    } else if (response->status_code == 403 && ErrorText(*response) == "Locker quota reached") {
      *refused = LockerPutRefused{.reason = LockerPutRefusal::QuotaReached};
    }
  }
  return Error("Locker put refused: " + ErrorText(*response));
}

Roe<std::optional<LockerRecord>> HttpLockerClient::Get(const std::string& locker_id) {
  if (base_url_.empty()) {
    return Error("Locker base_url not configured");
  }
  auto response = HttpClient::Get(base_url_ + "/v1/lockers/" + locker_id, WithRelayClientHeader());
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
  LockerRecord record = LockerRecordFromJson(*locker);
  if (record.locker_id != locker_id) {
    return Error("Locker get: another locker in the answer");
  }
  return std::optional<LockerRecord>(std::move(record));
}

} // namespace pbr
