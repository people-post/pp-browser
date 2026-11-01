#pragma once

// Test doubles for the org backend clients. Linked only into test binaries
// (pp_lib_net_testing); production code never sees these types.

#include "lib/net/OrgBackendClientsImpl.h"

#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
#include "common/PbrCompat.h"
#include "common/MeshAlias.h"

namespace pbr {

class MockBlobClient : public IBlobClient {
public:
  Roe<BlobPresignResult> Presign(const std::string& relay_user_id, const std::string& content_type,
                                 uint64_t byte_length, BlobPurpose purpose) override;
  Roe<void> Retain(const std::string& relay_user_id, const std::string& blob_id) override;
  Roe<void> Delete(const std::string& relay_user_id, const std::string& blob_id) override;
  Roe<BlobListResult> List(const std::string& relay_user_id, const std::string& status_filter = "") override;
  Roe<void> SetProfileIcon(const std::string& relay_user_id, const std::string& url, const std::string& blob_id,
                           const std::string& kind) override;
  Roe<void> PutUpload(const std::string& upload_url, const std::string& content_type,
                      const std::string& body) override;

  void SetPresignError(Error error) { presign_error_ = std::move(error); }
  void ClearPresignError() { presign_error_ = std::nullopt; }
  void SetListResult(BlobListResult result) { list_result_ = std::move(result); }

  const std::vector<std::string>& UploadedBodies() const { return uploaded_bodies_; }
  const std::vector<std::string>& RetainedBlobIds() const { return retained_blob_ids_; }
  const std::vector<std::string>& DeletedBlobIds() const { return deleted_blob_ids_; }
  uint32_t PresignCallCount() const { return presign_call_count_; }

private:
  std::vector<std::string> uploaded_bodies_;
  std::vector<std::string> retained_blob_ids_;
  std::vector<std::string> deleted_blob_ids_;
  std::optional<Error> presign_error_;
  std::optional<BlobListResult> list_result_;
  uint64_t next_blob_id_ = 1;
  uint32_t presign_call_count_ = 0;
};

class MockDirectoryClient : public mesh::IDirectoryClient {
public:
  void SetDefaultKemPublicKeyB64(std::string kem_public_key_b64) {
    default_kem_public_key_b64_ = std::move(kem_public_key_b64);
  }

  Roe<std::vector<mesh::DirectoryHit>> SearchPeople(const std::string& query) override;
  Roe<mesh::DirectoryHit> LookupRelayUser(const std::string& relay_user_id) override;
  Roe<mesh::DirectoryHit> LookupByAccount(const std::string& account_id) override;

private:
  std::string default_kem_public_key_b64_;
};

class MockRegistrationClient : public IRegistrationClient {
public:
  Roe<RegistrationStartResult> StartRegistration(const std::string& public_key_b64, const std::string& nickname,
                                                 const std::string& signature_alg = "ml-dsa-65",
                                                 const std::string& kem_public_key_b64 = "",
                                                 const std::string& peer_id = "",
                                                 const std::vector<std::string>& multiaddrs = {},
                                                 const RegistrationPublishOpts& publish = {}) override;
  Roe<RegistrationResult> FinishRegistration(const std::string& challenge, const std::string& public_key_b64,
                                             const std::string& nickname, const std::string& signature,
                                             int64_t timestamp, const std::string& signature_alg = "ml-dsa-65",
                                             const std::string& kem_public_key_b64 = "",
                                             const std::string& kem_binding_signature_b64 = "",
                                             const std::string& peer_id = "",
                                             const std::vector<std::string>& multiaddrs = {},
                                             int64_t initiation_floor = 0,
                                             const RegistrationPublishOpts& publish = {}) override;
  Roe<RegistrationResult> UpdateNickname(const std::string& new_nickname, const std::string& signature,
                                         int64_t timestamp, const std::string& relay_user_id) override;
  Roe<void> RevokeRegistration(const std::string& relay_user_id, const std::string& signature,
                               int64_t timestamp) override;
};

class MockClientCompatClient : public IClientCompatClient {
public:
  void SetDocument(ClientCompatDocument doc) {
    document_ = std::move(doc);
    has_document_ = true;
    error_.clear();
  }
  void SetError(std::string error) {
    error_ = std::move(error);
    has_document_ = false;
  }

  Roe<ClientCompatDocument> Fetch() override;

private:
  bool has_document_ = false;
  ClientCompatDocument document_;
  std::string error_;
};

} // namespace pbr
