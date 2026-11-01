#pragma once

#include "common/net/HttpTransport.h"
#include "lib/net/BlobClient.h"
#include "lib/net/OrgBackendClients.h"

#include <functional>
#include <string>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * Turn a blob-service HTTP response into a result. Server error text stays in the log detail; the
 * user-facing message comes from the error catalog (quota full, upload not configured, ...).
 */
Roe<void> CheckBlobResponse(const HttpResponse& response);

/**
 * The typed error for a failed blob-service answer: status code and the server's `error` text pick the
 * Err::Blob code; the log detail is "HTTP <status>: <text>".
 */
Error ClassifyBlobFailure(int status_code, const std::string& detail);

class HttpBlobClient : public IBlobClient {
public:
  explicit HttpBlobClient(std::string base_url);

  void SetAuthSigner(RelayAuthSigner signer) { auth_signer_ = std::move(signer); }

  Roe<BlobPresignResult> Presign(const std::string& relay_user_id, const std::string& content_type,
                                 uint64_t byte_length, BlobPurpose purpose) override;
  Roe<void> Retain(const std::string& relay_user_id, const std::string& blob_id) override;
  Roe<void> Delete(const std::string& relay_user_id, const std::string& blob_id) override;
  Roe<BlobListResult> List(const std::string& relay_user_id, const std::string& status_filter = "") override;
  Roe<void> SetProfileIcon(const std::string& relay_user_id, const std::string& url, const std::string& blob_id,
                           const std::string& kind) override;
  Roe<void> PutUpload(const std::string& upload_url, const std::string& content_type,
                      const std::string& body) override;
  Roe<void> PutUploadFile(const std::string& upload_url, const std::string& content_type, const std::string& path,
                          const std::function<bool(uint64_t done, uint64_t total)>& progress) override;

private:
  Roe<std::string> SignBytes(const std::vector<uint8_t>& sign_bytes) const;

  std::string base_url_;
  RelayAuthSigner auth_signer_;
};

} // namespace pbr
