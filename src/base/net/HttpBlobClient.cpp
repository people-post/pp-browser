#include "base/net/HttpBlobClient.h"

#include "base/net/HttpClient.h"
#include "base/net/RelayBlobSignPayload.h"
#include "common/Utilities.h"

#include <nlohmann/json.hpp>

namespace pbr {

namespace {

std::string HttpFailureMessage(const HttpResponse& response) {
  const nlohmann::json root = nlohmann::json::parse(response.body, nullptr, false);
  if (!root.is_discarded() && root.contains("error") && root["error"].is_string()) {
    return root["error"].get<std::string>();
  }
  return "HTTP request failed with status " + std::to_string(response.status_code);
}

Roe<void> ExpectSuccess(const HttpResponse& response) {
  if (response.status_code >= 200 && response.status_code < 300) {
    return Roe<void>{};
  }
  return Error(HttpFailureMessage(response));
}

RelayBlobRecord ParseBlobRecord(const nlohmann::json& item) {
  RelayBlobRecord record;
  if (item.contains("blob_id") && item["blob_id"].is_string()) {
    record.blob_id = item["blob_id"].get<std::string>();
  }
  if (item.contains("url") && item["url"].is_string()) {
    record.url = item["url"].get<std::string>();
  }
  if (item.contains("status") && item["status"].is_string()) {
    if (auto status = BlobStatusFromWire(item["status"].get<std::string>())) {
      record.status = *status;
    }
  }
  if (item.contains("tier") && item["tier"].is_string()) {
    if (auto tier = BlobTierFromWire(item["tier"].get<std::string>())) {
      record.tier = *tier;
    }
  }
  if (item.contains("content_type") && item["content_type"].is_string()) {
    record.content_type = item["content_type"].get<std::string>();
  }
  if (item.contains("byte_length") && item["byte_length"].is_number_unsigned()) {
    record.byte_length = item["byte_length"].get<uint64_t>();
  }
  if (item.contains("purpose") && item["purpose"].is_string()) {
    if (auto purpose = BlobPurposeFromWire(item["purpose"].get<std::string>())) {
      record.purpose = *purpose;
    }
  }
  if (item.contains("created_at") && item["created_at"].is_string()) {
    record.created_at = item["created_at"].get<std::string>();
  }
  if (item.contains("retained_at") && item["retained_at"].is_string()) {
    record.retained_at = item["retained_at"].get<std::string>();
  }
  if (item.contains("pending_expires_at") && item["pending_expires_at"].is_string()) {
    record.pending_expires_at = item["pending_expires_at"].get<std::string>();
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

  const nlohmann::json body = {{"relay_user_id", relay_user_id},
                               {"content_type", content_type},
                               {"byte_length", byte_length},
                               {"purpose", BlobPurposeToWire(purpose)},
                               {"timestamp", timestamp},
                               {"signature", *signature}};
  const auto response =
      HttpClient::Post(base_url_ + "/v1/blobs/presign", body.dump(), {{"Content-Type", "application/json"}});
  if (!response) {
    return response.error();
  }
  if (const auto ok = ExpectSuccess(response.value()); !ok) {
    return ok.error();
  }

  const nlohmann::json root = nlohmann::json::parse(response.value().body, nullptr, false);
  if (root.is_discarded() || !root.contains("blob_id") || !root.contains("upload_url") || !root.contains("public_url")) {
    return Error("Invalid blob presign JSON");
  }

  BlobPresignResult result;
  result.blob_id = root["blob_id"].get<std::string>();
  result.upload_url = root["upload_url"].get<std::string>();
  result.public_url = root["public_url"].get<std::string>();
  if (root.contains("tier") && root["tier"].is_string()) {
    if (auto tier = BlobTierFromWire(root["tier"].get<std::string>())) {
      result.tier = *tier;
    }
  }
  if (root.contains("pending_expires_at") && root["pending_expires_at"].is_string()) {
    result.pending_expires_at = root["pending_expires_at"].get<std::string>();
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

  const nlohmann::json body = {{"relay_user_id", relay_user_id},
                               {"blob_id", blob_id},
                               {"timestamp", timestamp},
                               {"signature", *signature}};
  const auto response =
      HttpClient::Post(base_url_ + "/v1/blobs/retain", body.dump(), {{"Content-Type", "application/json"}});
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

  const nlohmann::json body = {{"relay_user_id", relay_user_id},
                               {"blob_id", blob_id},
                               {"timestamp", timestamp},
                               {"signature", *signature}};
  const auto response =
      HttpClient::Post(base_url_ + "/v1/blobs/delete", body.dump(), {{"Content-Type", "application/json"}});
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

  nlohmann::json body = {{"relay_user_id", relay_user_id},
                         {"timestamp", timestamp},
                         {"signature", *signature}};
  if (!status_filter.empty()) {
    body["status"] = status_filter;
  }
  const auto response =
      HttpClient::Post(base_url_ + "/v1/blobs/list", body.dump(), {{"Content-Type", "application/json"}});
  if (!response) {
    return response.error();
  }
  if (const auto ok = ExpectSuccess(response.value()); !ok) {
    return ok.error();
  }

  const nlohmann::json root = nlohmann::json::parse(response.value().body, nullptr, false);
  if (root.is_discarded() || !root.contains("blobs") || !root["blobs"].is_array()) {
    return Error("Invalid blob list JSON");
  }

  BlobListResult result;
  for (const auto& item : root["blobs"]) {
    result.blobs.push_back(ParseBlobRecord(item));
  }
  if (root.contains("usage") && root["usage"].is_object()) {
    const auto& usage = root["usage"];
    if (usage.contains("small_count") && usage["small_count"].is_number_unsigned()) {
      result.usage.small_count = usage["small_count"].get<uint64_t>();
    }
    if (usage.contains("large_bytes") && usage["large_bytes"].is_number_unsigned()) {
      result.usage.large_bytes = usage["large_bytes"].get<uint64_t>();
    }
    if (usage.contains("large_included_bytes") && usage["large_included_bytes"].is_number_unsigned()) {
      result.usage.large_included_bytes = usage["large_included_bytes"].get<uint64_t>();
    }
    if (usage.contains("overage_bytes") && usage["overage_bytes"].is_number_unsigned()) {
      result.usage.overage_bytes = usage["overage_bytes"].get<uint64_t>();
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

  const nlohmann::json body = {{"relay_user_id", relay_user_id},
                               {"url", url},
                               {"blob_id", blob_id},
                               {"kind", kind},
                               {"timestamp", timestamp},
                               {"signature", *signature}};
  const auto response =
      HttpClient::Post(base_url_ + "/v1/profile/icon", body.dump(), {{"Content-Type", "application/json"}});
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
