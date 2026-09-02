#pragma once

#include "domain/net/BlobClient.h"
#include "domain/net/ServiceClients.h"

#include <functional>
#include <string>
#include "common/PbrCompat.h"

namespace pbr {

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

private:
  Roe<std::string> SignBytes(const std::vector<uint8_t>& sign_bytes) const;

  std::string base_url_;
  RelayAuthSigner auth_signer_;
};

} // namespace pbr
