#include "lib/net/HttpBlobClient.h"

#include "foundation/error/AppError.h"
#include "lib/net/HttpClient.h"
#include "lib/net/RelayBlobSignPayload.h"
#include "common/Utilities.h"
#include "common/ValueJson.h"
#include "common/PbrCompat.h"

#include <optional>
#include <string_view>

namespace pbr {

namespace {

std::string HttpFailureMessage(const HttpResponse& response) {
  if (auto root = TryParseObject(response.body)) {
    if (auto error = root->getString("error")) {
      return *error;
    }
  }
  return std::string();
}

/** The `/files` relay's machine-readable `code` (FILE_TOO_LARGE, QUOTA_FILE_BYTES, ...); "" when absent. */
std::string HttpFailureCode(const HttpResponse& response) {
  if (auto root = TryParseObject(response.body)) {
    if (auto code = root->getString("code")) {
      return *code;
    }
  }
  return std::string();
}

/** The typed error a relay `code` names; none for a code that has no better meaning than its status. */
std::optional<Err::Blob> BlobErrorOfRelayCode(const std::string& code) {
  if (code == "FILE_TOO_LARGE") {
    return Err::Blob::TooLarge;
  }
  if (code == "QUOTA_FILE_BYTES") {
    return Err::Blob::QuotaExceeded;
  }
  if (code == "QUOTA_PENDING_COUNT") {
    return Err::Blob::RateLimited;
  }
  if (code == "BLOB_NOT_FOUND") {
    return Err::Blob::Expired;
  }
  if (code == "UPLOAD_NOT_CONFIGURED") {
    return Err::Blob::UploadNotConfigured;
  }
  if (code == "BLOB_NOT_OWNED") {
    return Err::Blob::UploadRejected;
  }
  if (code == "INVALID_BYTE_LENGTH" || code == "BLOB_NOT_PENDING" || code == "UPLOAD_INCOMPLETE" ||
      code == "INVALID_PART_NUMBER") {
    return Err::Blob::Generic;
  }
  return std::nullopt;
}

RelayFileRecord ParseFileRecord(const Object& item) {
  RelayFileRecord record;
  record.blob_id = item.getString("blob_id").value_or("");
  record.status = item.getString("status").value_or("");
  record.byte_length = item.getNonNegInt("byte_length").value_or(0);
  record.created_at = item.getString("created_at").value_or("");
  record.completed_at = item.getString("completed_at").value_or("");
  record.expires_at = item.getString("expires_at").value_or("");
  record.pending_expires_at = item.getString("pending_expires_at").value_or("");
  return record;
}

} // namespace

Error ClassifyBlobFailure(const int status_code, const std::string& detail, const std::string& code) {
  const std::string logged =
      "HTTP " + std::to_string(status_code) + (code.empty() ? "" : " " + code) + (detail.empty() ? "" : ": " + detail);
  if (const auto typed = BlobErrorOfRelayCode(code)) {
    return AppError::Blob(*typed, logged);
  }
  // The relay answers 503 `{ "error": "Blob upload is not configured" }` when its bucket is unset
  // (an older gateway used 400, B77): the text, not the status, tells this apart from "down".
  if (detail == "Blob upload is not configured") {
    return AppError::Blob(Err::Blob::UploadNotConfigured, logged);
  }
  switch (status_code) {
    case 429:
      // The same status carries three causes; only the relay's two quota texts mean a full space (freeing space
      // helps). Any other 429 (its rate limit, too many pending uploads, a proxy or WAF page) must never lead to
      // an offer to delete the oldest file.
      if (detail == "Small file quota exceeded" || detail == "Large file storage quota exceeded") {
        return AppError::Blob(Err::Blob::QuotaExceeded, logged);
      }
      return AppError::Blob(Err::Blob::RateLimited, logged);
    case 413:
      return AppError::Blob(Err::Blob::TooLarge, logged);
    case 403:
      return AppError::Blob(Err::Blob::UploadRejected, logged);
    case 404:
      return AppError::Blob(Err::Blob::Expired, logged);
    default:
      break;
  }
  if (status_code == 400 && detail == "File exceeds size limit") {
    return AppError::Blob(Err::Blob::TooLarge, logged);
  }
  if (status_code >= 500) {
    return AppError::Blob(Err::Blob::Unavailable, logged);
  }
  return AppError::Blob(Err::Blob::Generic, logged);
}

static Roe<void> CheckPutStatus(const int status) {
  if (status >= 200 && status < 300) {
    return Roe<void>{};
  }
  // The storage service, not the relay, answers a PUT: 403 is an expired or mismatched signed URL.
  if (status == 403) {
    return AppError::Blob(Err::Blob::UploadRejected, "Blob PUT failed with status 403");
  }
  if (status >= 500) {
    return AppError::Blob(Err::Blob::Unavailable, "Blob PUT failed with status " + std::to_string(status));
  }
  return Error("Blob PUT failed with status " + std::to_string(status));
}

Roe<void> CheckBlobResponse(const HttpResponse& response) {
  if (response.status_code >= 200 && response.status_code < 300) {
    return Roe<void>{};
  }
  return ClassifyBlobFailure(response.status_code, HttpFailureMessage(response), HttpFailureCode(response));
}

std::string RelayOriginOf(const std::string& relay_base_url) {
  std::string base = relay_base_url;
  while (!base.empty() && base.back() == '/') {
    base.pop_back();
  }
  constexpr std::string_view kRelayPath = "/api/relay";
  if (base.size() > kRelayPath.size() &&
      base.compare(base.size() - kRelayPath.size(), kRelayPath.size(), kRelayPath) == 0) {
    return base.substr(0, base.size() - kRelayPath.size());
  }
  // Any other base: its scheme and authority.
  const size_t scheme = base.find("://");
  const size_t path = scheme == std::string::npos ? std::string::npos : base.find('/', scheme + 3);
  return path == std::string::npos ? base : base.substr(0, path);
}

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
  if (const auto ok = CheckBlobResponse(response.value()); !ok) {
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
  return CheckBlobResponse(response.value());
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
  return CheckBlobResponse(response.value());
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
  return CheckBlobResponse(response.value());
}

Roe<HttpResponse> HttpBlobClient::PostSignedFiles(const std::string& op, const std::string& relay_user_id, Object body,
                                                  const std::vector<uint8_t>& sign_bytes,
                                                  const int64_t timestamp) const {
  if (base_url_.empty()) {
    return Error("Blob base_url not configured");
  }
  auto signature = SignBytes(sign_bytes);
  if (!signature) {
    return signature.error();
  }
  body.set("relay_user_id", relay_user_id);
  body.set("timestamp", timestamp);
  body.set("signature", *signature);
  auto response =
      HttpClient::Post(base_url_ + "/v1/files/" + op, DumpJson(body), {{"Content-Type", "application/json"}});
  if (!response) {
    return response.error();
  }
  if (const auto ok = CheckBlobResponse(response.value()); !ok) {
    return ok.error();
  }
  return response;
}

Roe<RelayFileStartResult> HttpBlobClient::FileStart(const std::string& relay_user_id, const uint64_t byte_length) {
  if (byte_length == 0) {
    return Error("byte_length must be positive");
  }
  const int64_t timestamp = util::NowUnixMs();
  Object body;
  body.setJsonUInt("byte_length", byte_length);
  auto response = PostSignedFiles("start", relay_user_id, std::move(body),
                                  BuildFileStartSignBytes(relay_user_id, byte_length, timestamp), timestamp);
  if (!response) {
    Error error = response.error();
    error.message += " byte_length=" + std::to_string(byte_length); // a full space is planned against this size
    return error;
  }
  auto root = TryParseObject(response->body);
  if (!root || !root->getString("blob_id") || !root->getString("mode")) {
    return Error("Invalid relay file start JSON");
  }
  RelayFileStartResult result;
  result.blob_id = *root->getString("blob_id");
  result.mode = *root->getString("mode");
  result.upload_url = root->getString("upload_url").value_or("");
  // Missing or not positive: 0, refused (and the blob deleted) by UploadRelayFile as a plan missing them.
  result.part_bytes = root->getNonNegInt("part_bytes").value_or(0);
  result.part_count = root->getNonNegInt("part_count").value_or(0);
  result.expires_in = static_cast<int64_t>(root->getNonNegInt("expires_in").value_or(0));
  result.pending_expires_at = root->getString("pending_expires_at").value_or("");
  if (result.blob_id.empty()) {
    return Error("Invalid relay file start JSON");
  }
  return result;
}

Roe<RelayFilePartResult> HttpBlobClient::FilePart(const std::string& relay_user_id, const std::string& blob_id,
                                                  const uint64_t part_number) {
  const int64_t timestamp = util::NowUnixMs();
  Object body;
  body.set("blob_id", blob_id);
  body.setJsonUInt("part_number", part_number);
  auto response = PostSignedFiles("part", relay_user_id, std::move(body),
                                  BuildFilePartSignBytes(relay_user_id, blob_id, part_number, timestamp), timestamp);
  if (!response) {
    return response.error();
  }
  auto root = TryParseObject(response->body);
  // Plain assignments rather than `root ? … : std::nullopt`: GCC 13 reports the optional as maybe-uninitialized
  // through the conditional (-Werror=maybe-uninitialized).
  std::optional<std::string> upload_url;
  std::optional<uint64_t> part_bytes;
  if (root) {
    upload_url = root->getString("upload_url");
    part_bytes = root->getNonNegInt("part_bytes");
  }
  if (!upload_url.has_value() || upload_url->empty()) {
    return Error("Invalid relay file part JSON");
  }
  if (!part_bytes.has_value() || part_bytes.value() == 0) {
    return Error("relay file part missing part_bytes");
  }
  RelayFilePartResult result;
  result.upload_url = upload_url.value();
  result.part_bytes = part_bytes.value();
  result.expires_in = static_cast<int64_t>(root->getNonNegInt("expires_in").value_or(0));
  return result;
}

Roe<RelayFileCompleteResult> HttpBlobClient::FileComplete(const std::string& relay_user_id,
                                                          const std::string& blob_id) {
  const int64_t timestamp = util::NowUnixMs();
  Object body;
  body.set("blob_id", blob_id);
  auto response = PostSignedFiles("complete", relay_user_id, std::move(body),
                                  BuildFileCompleteSignBytes(relay_user_id, blob_id, timestamp), timestamp);
  if (!response) {
    return response.error();
  }
  auto root = TryParseObject(response->body);
  const auto share_path = root ? root->getString("share_path") : std::nullopt;
  if (!share_path || share_path->empty() || share_path->front() != '/') {
    return Error("Invalid relay file complete JSON");
  }
  RelayFileCompleteResult result;
  result.blob_id = root->getString("blob_id").value_or(blob_id);
  result.share_path = *share_path;
  result.share_url = RelayOriginOf(base_url_) + *share_path;
  result.expires_at = root->getString("expires_at").value_or("");
  return result;
}

Roe<void> HttpBlobClient::FileDelete(const std::string& relay_user_id, const std::string& blob_id) {
  if (blob_id.empty()) {
    return Error("blob_id is required");
  }
  const int64_t timestamp = util::NowUnixMs();
  Object body;
  body.set("blob_id", blob_id);
  auto response = PostSignedFiles("delete", relay_user_id, std::move(body),
                                  BuildFileDeleteSignBytes(relay_user_id, blob_id, timestamp), timestamp);
  if (!response) {
    return response.error();
  }
  return Roe<void>{};
}

Roe<RelayFileListResult> HttpBlobClient::FileList(const std::string& relay_user_id, const std::string& status_filter) {
  const int64_t timestamp = util::NowUnixMs();
  Object body;
  if (!status_filter.empty()) {
    body.set("status", status_filter);
  }
  auto response = PostSignedFiles("list", relay_user_id, std::move(body),
                                  BuildFileListSignBytes(relay_user_id, status_filter, timestamp), timestamp);
  if (!response) {
    return response.error();
  }
  auto root = TryParseObject(response->body);
  const Array* files = root ? root->getArray("files") : nullptr;
  if (!files) {
    return Error("Invalid relay file list JSON");
  }
  RelayFileListResult result;
  for (const Value& item_value : files->elements) {
    if (const Object* item = asObject(item_value)) {
      result.files.push_back(ParseFileRecord(*item));
    }
  }
  if (const Object* usage = root->getObject("usage")) {
    result.usage.bytes_used = usage->getNonNegInt("bytes_used").value_or(0);
    result.usage.quota_bytes = usage->getNonNegInt("quota_bytes").value_or(0);
    result.usage.pending_count = usage->getNonNegInt("pending_count").value_or(0);
    result.usage.max_pending = usage->getNonNegInt("max_pending").value_or(0);
  }
  return result;
}

Roe<void> HttpBlobClient::PutUploadFile(const std::string& upload_url, const std::string& content_type,
                                        const std::string& path,
                                        const std::function<bool(uint64_t, uint64_t)>& progress) {
  if (upload_url.empty()) {
    return Error("upload_url is required");
  }
  const auto response = HttpClient::PutFile(upload_url, path, {{"Content-Type", content_type}}, progress);
  if (!response) {
    return response.error();
  }
  return CheckPutStatus(response.value().status_code);
}

Roe<void> HttpBlobClient::PutUploadFileRange(const std::string& upload_url, const std::string& content_type,
                                             const std::string& path, const uint64_t offset, const uint64_t length,
                                             const std::function<bool(uint64_t, uint64_t)>& progress) {
  if (upload_url.empty()) {
    return Error("upload_url is required");
  }
  const auto response = HttpClient::PutFileRange(upload_url, path, offset, length, content_type, progress);
  if (!response) {
    return response.error();
  }
  return CheckPutStatus(static_cast<int>(response.value().status_code));
}

Roe<void> HttpBlobClient::PutUpload(const std::string& upload_url, const std::string& content_type,
                                    const std::string& body) {
  if (upload_url.empty()) {
    return Error("upload_url is required");
  }
  const auto response = HttpClient::Put(upload_url, body, {{"Content-Type", content_type}}, kMaxHttpClientBodyBytes, {},
                                        /*restrict_to_public_https=*/false, /*add_content_sha256=*/false);
  if (!response) {
    return response.error();
  }
  return CheckPutStatus(static_cast<int>(response.value().status_code));
}

} // namespace pbr
