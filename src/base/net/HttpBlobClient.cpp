#include "base/net/HttpBlobClient.h"

#include "base/error/AppError.h"
#include "base/net/HttpClient.h"
#include "base/net/RelayBlobSignPayload.h"
#include "common/Utilities.h"
#include "common/ValueJson.h"

namespace pbr {

namespace {

std::string HttpFailureMessage(const HttpResponse& response) {
  if (auto root = TryParseObject(response.body)) {
    if (auto error = root->getString("error")) {
      return *error;
    }
  }
  return "HTTP request failed with status " + std::to_string(response.status_code);
}

Roe<void> ExpectSuccess(const HttpResponse& response) {
  if (response.status_code >= 200 && response.status_code < 300) {
    return Roe<void>{};
  }
  if (response.status_code == 429) {
    return AppError::Blob(Err::Blob::QuotaExceeded, HttpFailureMessage(response));
  }
  return Error(HttpFailureMessage(response));
}

RelayBlobRecord ParseBlobRecord(const Object& item) {
  RelayBlobRecord record;
  if (auto v = item.getString("blob_id")) {
    record.blob_id = *v;
  }
  if (auto v = item.getString("url")) {
    record.url = *v;
  }
  if (auto v = item.getString("status")) {
    if (auto status = BlobStatusFromWire(*v)) {
      record.status = *status;
    }
  }
  if (auto v = item.getString("tier")) {
    if (auto tier = BlobTierFromWire(*v)) {
      record.tier = *tier;
    }
  }
  if (auto v = item.getString("content_type")) {
    record.content_type = *v;
  }
  if (auto v = item.getNonNegInt("byte_length")) {
    record.byte_length = *v;
  }
  if (auto v = item.getString("purpose")) {
    if (auto purpose = BlobPurposeFromWire(*v)) {
      record.purpose = *purpose;
    }
  }
  if (auto v = item.getString("created_at")) {
    record.created_at = *v;
  }
  if (auto v = item.getString("retained_at")) {
    record.retained_at = *v;
  }
  if (auto v = item.getString("pending_expires_at")) {
    record.pending_expires_at = *v;
  }
  return record;
}

} // namespace

HttpBlobClient::HttpBlobClient(std::string base_url) : base_url_(std::move(base_url)) {}

Roe<std::string> HttpBlobClient::SignBytes(const std::vector<uint8_t>& sign_bytes) const {
  if (!auth_signer_) {
    return Error("Blob auth signer not configured");
  }
  if (sign_bytes.empty()) {
    return Error("Failed to build blob sign bytes");
  }
  return auth_signer_(sign_bytes);
}

Roe<BlobPresignResult> HttpBlobClient::Presign(const std::string& relay_user_id, const std::string& content_type,
                                               const uint64_t byte_length, const BlobPurpose purpose) {
  if (base_url_.empty()) {
    return Error("Blob base_url not configured");
  }
  if (byte_length == 0) {
    return Error("byte_length must be positive");
  }

  const int64_t timestamp = util::NowUnixMs();
  const auto sign_bytes =
      BuildBlobPresignSignBytes(relay_user_id, content_type, byte_length, BlobPurposeToWire(purpose), timestamp);
  auto signature = SignBytes(sign_bytes);
  if (!signature) {
    return signature.error();
  }

  Object body;
  body.set("relay_user_id", relay_user_id);
  body.set("content_type", content_type);
  body.setJsonUInt("byte_length", byte_length);
  body.set("purpose", BlobPurposeToWire(purpose));
  body.set("timestamp", timestamp);
  body.set("signature", *signature);
  const auto response =
      HttpClient::Post(base_url_ + "/v1/blobs/presign", DumpJson(body), {{"Content-Type", "application/json"}});
  if (!response) {
    return response.error();
  }
  if (const auto ok = ExpectSuccess(response.value()); !ok) {
    return ok.error();
  }

  auto root = TryParseObject(response.value().body);
  if (!root || !root->contains("blob_id") || !root->contains("upload_url") || !root->contains("public_url")) {
    return Error("Invalid blob presign JSON");
  }

  BlobPresignResult result;
  result.blob_id = root->getString("blob_id").value_or("");
  result.upload_url = root->getString("upload_url").value_or("");
  result.public_url = root->getString("public_url").value_or("");
  if (auto tier = root->getString("tier")) {
    if (auto parsed = BlobTierFromWire(*tier)) {
      result.tier = *parsed;
    }
  }
  if (auto expires = root->getString("pending_expires_at")) {
    result.pending_expires_at = *expires;
  }
  return result;
}

Roe<void> HttpBlobClient::Retain(const std::string& relay_user_id, const std::string& blob_id) {
  if (base_url_.empty()) {
    return Error("Blob base_url not configured");
  }
  const int64_t timestamp = util::NowUnixMs();
  const auto sign_bytes = BuildBlobRetainSignBytes(relay_user_id, blob_id, timestamp);
  auto signature = SignBytes(sign_bytes);
  if (!signature) {
    return signature.error();
  }

  Object body;
  body.set("relay_user_id", relay_user_id);
  body.set("blob_id", blob_id);
  body.set("timestamp", timestamp);
  body.set("signature", *signature);
  const auto response =
      HttpClient::Post(base_url_ + "/v1/blobs/retain", DumpJson(body), {{"Content-Type", "application/json"}});
  if (!response) {
    return response.error();
  }
  return ExpectSuccess(response.value());
}

Roe<void> HttpBlobClient::Delete(const std::string& relay_user_id, const std::string& blob_id) {
  if (base_url_.empty()) {
    return Error("Blob base_url not configured");
  }
  const int64_t timestamp = util::NowUnixMs();
  const auto sign_bytes = BuildBlobDeleteSignBytes(relay_user_id, blob_id, timestamp);
  auto signature = SignBytes(sign_bytes);
  if (!signature) {
    return signature.error();
  }

  Object body;
  body.set("relay_user_id", relay_user_id);
  body.set("blob_id", blob_id);
  body.set("timestamp", timestamp);
  body.set("signature", *signature);
  const auto response =
      HttpClient::Post(base_url_ + "/v1/blobs/delete", DumpJson(body), {{"Content-Type", "application/json"}});
  if (!response) {
    return response.error();
  }
  return ExpectSuccess(response.value());
}

Roe<BlobListResult> HttpBlobClient::List(const std::string& relay_user_id, const std::string& status_filter) {
  if (base_url_.empty()) {
    return Error("Blob base_url not configured");
  }
  const int64_t timestamp = util::NowUnixMs();
  const auto sign_bytes = BuildBlobListSignBytes(relay_user_id, status_filter, timestamp);
  auto signature = SignBytes(sign_bytes);
  if (!signature) {
    return signature.error();
  }

  Object body;
  body.set("relay_user_id", relay_user_id);
  body.set("timestamp", timestamp);
  body.set("signature", *signature);
  if (!status_filter.empty()) {
    body.set("status", status_filter);
  }
  const auto response =
      HttpClient::Post(base_url_ + "/v1/blobs/list", DumpJson(body), {{"Content-Type", "application/json"}});
  if (!response) {
    return response.error();
  }
  if (const auto ok = ExpectSuccess(response.value()); !ok) {
    return ok.error();
  }

  auto root = TryParseObject(response.value().body);
  const Array* blobs = root ? root->getArray("blobs") : nullptr;
  if (!blobs) {
    return Error("Invalid blob list JSON");
  }

  BlobListResult result;
  for (const Value& item_value : blobs->elements) {
    const Object* item = asObject(item_value);
    if (!item) {
      continue;
    }
    result.blobs.push_back(ParseBlobRecord(*item));
  }
  if (const Object* usage = root->getObject("usage")) {
    if (auto v = usage->getNonNegInt("small_count")) {
      result.usage.small_count = *v;
    }
    if (auto v = usage->getNonNegInt("large_bytes")) {
      result.usage.large_bytes = *v;
    }
    if (auto v = usage->getNonNegInt("large_included_bytes")) {
      result.usage.large_included_bytes = *v;
    }
    if (auto v = usage->getNonNegInt("overage_bytes")) {
      result.usage.overage_bytes = *v;
    }
  }
  return result;
}

Roe<void> HttpBlobClient::SetProfileIcon(const std::string& relay_user_id, const std::string& url,
                                         const std::string& blob_id, const std::string& kind) {
  if (base_url_.empty()) {
    return Error("Blob base_url not configured");
  }
  const int64_t timestamp = util::NowUnixMs();
  const auto sign_bytes = BuildProfileIconSignBytes(relay_user_id, url, blob_id, kind, timestamp);
  auto signature = SignBytes(sign_bytes);
  if (!signature) {
    return signature.error();
  }

  Object body;
  body.set("relay_user_id", relay_user_id);
  body.set("url", url);
  body.set("blob_id", blob_id);
  body.set("kind", kind);
  body.set("timestamp", timestamp);
  body.set("signature", *signature);
  const auto response =
      HttpClient::Post(base_url_ + "/v1/profile/icon", DumpJson(body), {{"Content-Type", "application/json"}});
  if (!response) {
    return response.error();
  }
  return ExpectSuccess(response.value());
}

Roe<void> HttpBlobClient::PutUpload(const std::string& upload_url, const std::string& content_type,
                                    const std::string& body) {
  if (upload_url.empty()) {
    return Error("upload_url is required");
  }
  const auto response = HttpClient::Put(upload_url, body, {{"Content-Type", content_type}});
  if (!response) {
    return response.error();
  }
  if (response.value().status_code < 200 || response.value().status_code >= 300) {
    return Error("Blob PUT failed with status " + std::to_string(response.value().status_code));
  }
  return Roe<void>{};
}

} // namespace pbr
