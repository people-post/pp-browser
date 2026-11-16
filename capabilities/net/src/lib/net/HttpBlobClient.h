#pragma once

#include "common/net/HttpTransport.h"
#include "lib/net/BlobClient.h"
#include "lib/net/OrgBackendClients.h"
#include "lib/net/RelayAudience.h"

#include <memory>

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
 * The typed error for a failed blob-service answer. The `/files` relay's `code` decides when there is one; else
 * the status code and the server's `error` text (the `/blobs` calls carry no code). The log detail is
 * "HTTP <status>: <text>", or "HTTP <status> <CODE>: <text>" with a code (RelayFileErrorCode reads it back).
 */
Error ClassifyBlobFailure(int status_code, const std::string& detail, const std::string& code = "");

/** The relay origin (`https://host`) the files relay's `share_path` is relative to, from the relay base URL. */
std::string RelayOriginOf(const std::string& relay_base_url);

class HttpBlobClient : public IBlobClient {
public:
  /** `public_only`: the relay's own calls go over https to a public address only (a relay the person added). */
  explicit HttpBlobClient(std::string base_url, bool public_only = false);

  void SetAuthSigner(RelayAuthSigner signer) { auth_signer_ = std::move(signer); }
  /** Who the signed calls are for (the relay's pin, MR007); refusals about its identity or our registration go there. */
  void SetAudience(std::shared_ptr<IRelayAudience> audience) { audience_ = std::move(audience); }

  Roe<BlobPresignResult> Presign(const std::string& account_id, const std::string& content_type, uint64_t byte_length,
                                 BlobPurpose purpose) override;
  Roe<void> Retain(const std::string& account_id, const std::string& blob_id) override;
  Roe<void> Delete(const std::string& account_id, const std::string& blob_id) override;
  Roe<void> SetProfileIcon(const std::string& account_id, const std::string& url, const std::string& blob_id,
                           const std::string& kind) override;
  Roe<void> PutUpload(const std::string& upload_url, const std::string& content_type, const std::string& body) override;
  Roe<void> PutUploadFile(const std::string& upload_url, const std::string& content_type, const std::string& path,
                          const std::function<bool(uint64_t done, uint64_t total)>& progress) override;

  Roe<void> PutUploadFileRange(const std::string& upload_url, const std::string& content_type, const std::string& path,
                               uint64_t offset, uint64_t length,
                               const std::function<bool(uint64_t done, uint64_t total)>& progress) override;

  Roe<RelayFileStartResult> FileStart(const std::string& account_id, uint64_t byte_length) override;
  Roe<RelayFilePartResult> FilePart(const std::string& account_id, const std::string& blob_id,
                                    uint64_t part_number) override;
  Roe<RelayFileCompleteResult> FileComplete(const std::string& account_id, const std::string& blob_id) override;
  Roe<void> FileDelete(const std::string& account_id, const std::string& blob_id) override;
  Roe<RelayFileListResult> FileList(const std::string& account_id, const std::string& status_filter) override;

private:
  /**
   * POST a signed body to `path` (`/v1/blobs/presign`, `/v1/files/start`, ...): the audience, timestamp and signature
   * over the bytes `build` makes are added here. A refusal is the blob service's typed error (CheckBlobResponse); the
   * audience hears the codes it acts on first.
   */
  Roe<HttpResponse> PostSigned(const std::string& path, Object body, int64_t timestamp,
                               const RelayCommandBytes& build) const;

  std::string base_url_;
  bool public_only_ = false;
  RelayAuthSigner auth_signer_;
  std::shared_ptr<IRelayAudience> audience_;
};

} // namespace pbr
