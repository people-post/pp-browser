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
  Roe<void> SetProfileIcon(const std::string& relay_user_id, const std::string& url, const std::string& blob_id,
                           const std::string& kind) override;
  Roe<void> PutUpload(const std::string& upload_url, const std::string& content_type, const std::string& body) override;

  Roe<RelayFileStartResult> FileStart(const std::string& relay_user_id, uint64_t byte_length) override;
  Roe<RelayFilePartResult> FilePart(const std::string& relay_user_id, const std::string& blob_id,
                                    uint64_t part_number) override;
  Roe<RelayFileCompleteResult> FileComplete(const std::string& relay_user_id, const std::string& blob_id) override;
  Roe<void> FileDelete(const std::string& relay_user_id, const std::string& blob_id) override;
  Roe<RelayFileListResult> FileList(const std::string& relay_user_id, const std::string& status_filter) override;

  void SetPresignError(Error error) { presign_error_ = std::move(error); }
  void ClearPresignError() { presign_error_ = std::nullopt; }

  // Relay files: scripted answers (each error stays until cleared), and what was asked.
  void SetFileStartError(Error error) { file_start_error_ = std::move(error); }
  void ClearFileStartError() { file_start_error_ = std::nullopt; }
  /** The next `count` FileStart calls fail with `error`, then they succeed again. */
  void FailNextFileStarts(Error error, uint32_t count) {
    file_start_error_ = std::move(error);
    file_start_failures_left_ = count;
  }
  void SetFileStartMode(std::string mode) { file_start_mode_ = std::move(mode); }
  /** The part size FileStart plans with (multipart when a file is larger). */
  void SetFilePartBytes(uint64_t part_bytes) { file_part_bytes_ = part_bytes; }
  void SetPutError(Error error) { put_error_ = std::move(error); }
  void ClearPutError() { put_error_ = std::nullopt; }
  void SetFileCompleteError(Error error) { file_complete_error_ = std::move(error); }
  void SetFileListResult(RelayFileListResult result) { file_list_result_ = std::move(result); }
  /** Called inside FileComplete before it answers (a test can break the outgoing store there). */
  void SetOnFileComplete(std::function<void(const std::string& blob_id)> hook) { on_file_complete_ = std::move(hook); }

  const std::vector<std::string>& UploadedBodies() const { return uploaded_bodies_; }
  const std::vector<std::string>& RetainedBlobIds() const { return retained_blob_ids_; }
  const std::vector<std::string>& DeletedBlobIds() const { return deleted_blob_ids_; }
  uint32_t PresignCallCount() const { return presign_call_count_; }
  const std::vector<uint64_t>& FileStartByteLengths() const { return file_start_byte_lengths_; }
  uint32_t FileStartCallCount() const { return static_cast<uint32_t>(file_start_byte_lengths_.size()); }
  const std::vector<std::string>& CompletedFileIds() const { return completed_file_ids_; }
  const std::vector<std::string>& DeletedFileIds() const { return deleted_file_ids_; }
  const std::vector<std::string>& FileListFilters() const { return file_list_filters_; }

private:
  std::vector<std::string> uploaded_bodies_;
  std::vector<std::string> retained_blob_ids_;
  std::vector<std::string> deleted_blob_ids_;
  std::optional<Error> presign_error_;
  uint64_t next_blob_id_ = 1;
  uint32_t presign_call_count_ = 0;

  std::optional<Error> file_start_error_;
  uint32_t file_start_failures_left_ = 0; // 0: file_start_error_ (if any) stays
  std::string file_start_mode_ = "single";
  uint64_t file_part_bytes_ = 16ull * 1024 * 1024;
  std::unordered_map<std::string, uint64_t> file_byte_lengths_;
  std::optional<Error> put_error_;
  std::optional<Error> file_complete_error_;
  std::optional<RelayFileListResult> file_list_result_;
  std::function<void(const std::string&)> on_file_complete_;
  std::vector<uint64_t> file_start_byte_lengths_;
  std::vector<std::string> completed_file_ids_;
  std::vector<std::string> deleted_file_ids_;
  std::vector<std::string> file_list_filters_;
  uint64_t next_file_id_ = 1;
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
  std::string BaseUrl() const override { return base_url_; }
  void SetBaseUrl(std::string base_url) { base_url_ = std::move(base_url); }
  Roe<RegistrationStartResult> StartRegistration(const std::string& public_key_b64, const std::string& nickname,
                                                 const std::string& signature_alg = "ml-dsa-65",
                                                 const std::string& kem_public_key_b64 = "",
                                                 const std::string& peer_id = "",
                                                 const std::vector<std::string>& multiaddrs = {},
                                                 const RegistrationPublishOpts& publish = {}) override;
  Roe<RegistrationResult> FinishRegistration(
      const std::string& challenge, const std::string& public_key_b64, const std::string& nickname,
      const std::string& signature, int64_t timestamp, const std::string& signature_alg = "ml-dsa-65",
      const std::string& kem_public_key_b64 = "", const std::string& kem_binding_signature_b64 = "",
      const std::string& peer_id = "", const std::vector<std::string>& multiaddrs = {}, int64_t initiation_floor = 0,
      const RegistrationPublishOpts& publish = {}) override;
  Roe<RegistrationResult> UpdateNickname(const std::string& new_nickname, const std::string& signature,
                                         int64_t timestamp, const std::string& relay_user_id) override;
  Roe<void> RevokeRegistration(const std::string& relay_user_id, const std::string& signature,
                               int64_t timestamp) override;

private:
  std::string base_url_;
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
