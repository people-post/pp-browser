#include "lib/net/testing/MockOrgBackendClients.h"

#include "common/Utilities.h"
#include "foundation/crypto/CryptoUtil.h"
#include "foundation/crypto/HybridKem.h"
#include "foundation/crypto/MlDsa.h"

#include <algorithm>
#include <mutex>
#include "common/PbrCompat.h"
#include "common/MeshAlias.h"

namespace pbr {

namespace {

std::optional<std::string> MockPeerKemPublicKeyB64() {
  static std::once_flag once;
  static std::string value;
  std::call_once(once, []() {
    auto keys = HybridKem::GenerateKeyPair();
    if (keys) {
      value = Base64Encode(keys->public_key);
    }
  });
  if (value.empty()) {
    return std::nullopt;
  }
  return value;
}

} // namespace

Roe<BlobPresignResult> MockBlobClient::Presign(const std::string& /*relay_user_id*/,
                                                 const std::string& /*content_type*/, const uint64_t byte_length,
                                                 const BlobPurpose /*purpose*/) {
  ++presign_call_count_;
  if (presign_error_) {
    return presign_error_.value();
  }
  BlobPresignResult result;
  result.blob_id = "mock-blob-" + std::to_string(next_blob_id_++);
  result.upload_url = "https://mock.example/upload/" + result.blob_id;
  result.public_url = "https://mock.example/public/" + result.blob_id;
  result.pending_expires_at = "2099-01-01T00:00:00.000Z";
  return result;
}

Roe<void> MockBlobClient::Retain(const std::string& /*relay_user_id*/, const std::string& blob_id) {
  retained_blob_ids_.push_back(blob_id);
  return Roe<void>{};
}

Roe<void> MockBlobClient::Delete(const std::string& /*relay_user_id*/, const std::string& blob_id) {
  deleted_blob_ids_.push_back(blob_id);
  return Roe<void>{};
}

Roe<void> MockBlobClient::SetProfileIcon(const std::string& /*relay_user_id*/, const std::string& /*url*/,
                                         const std::string& /*blob_id*/, const std::string& /*kind*/) {
  return Roe<void>{};
}

Roe<void> MockBlobClient::PutUpload(const std::string& /*upload_url*/, const std::string& /*content_type*/,
                                    const std::string& body) {
  if (put_error_) {
    return *put_error_;
  }
  uploaded_bodies_.push_back(body);
  return Roe<void>{};
}

Roe<RelayFileStartResult> MockBlobClient::FileStart(const std::string& /*relay_user_id*/, const uint64_t byte_length) {
  file_start_byte_lengths_.push_back(byte_length);
  if (file_start_error_) {
    Error error = *file_start_error_;
    if (file_start_failures_left_ > 0 && --file_start_failures_left_ == 0) {
      file_start_error_ = std::nullopt;
    }
    return error;
  }
  RelayFileStartResult result;
  result.blob_id = "00000000-0000-4000-8000-" + std::string(12 - std::to_string(next_file_id_).size(), '0') +
                   std::to_string(next_file_id_);
  ++next_file_id_;
  result.mode = file_start_mode_;
  result.upload_url = "https://mock.example/put/" + result.blob_id + "?X-Amz-Signature=mock";
  result.part_bytes = file_part_bytes_;
  result.part_count = result.mode == "multipart" ? (byte_length + file_part_bytes_ - 1) / file_part_bytes_ : 1;
  file_byte_lengths_[result.blob_id] = byte_length;
  result.expires_in = 900;
  result.pending_expires_at = "2099-01-01T00:00:00.000Z";
  return result;
}

Roe<RelayFilePartResult> MockBlobClient::FilePart(const std::string& /*relay_user_id*/, const std::string& blob_id,
                                                  const uint64_t part_number) {
  const auto found = file_byte_lengths_.find(blob_id);
  const uint64_t byte_length = found == file_byte_lengths_.end() ? 0 : found->second;
  const uint64_t offset = (part_number - 1) * file_part_bytes_;
  if (part_number == 0 || offset >= byte_length) {
    return Error("HTTP 400 INVALID_PART_NUMBER: Invalid part number");
  }
  RelayFilePartResult result;
  result.upload_url = "https://mock.example/part/" + blob_id + "/" + std::to_string(part_number);
  result.part_bytes = std::min(file_part_bytes_, byte_length - offset);
  result.expires_in = 3600;
  return result;
}

Roe<RelayFileCompleteResult> MockBlobClient::FileComplete(const std::string& /*relay_user_id*/,
                                                          const std::string& blob_id) {
  if (on_file_complete_) {
    on_file_complete_(blob_id);
  }
  if (file_complete_error_) {
    return *file_complete_error_;
  }
  completed_file_ids_.push_back(blob_id);
  RelayFileCompleteResult result;
  result.blob_id = blob_id;
  // A token of the relay's shape (base64url, 43 chars): what the redaction tests must never see in a log.
  result.share_path =
      "/api/relay/v1/files/" + blob_id + "/download?t=Mock_Token-0123456789abcdefghijklmnopqrstuvwxyzAB";
  result.share_url = "https://mock.example" + result.share_path;
  result.expires_at = "2099-01-08T00:00:00.000Z";
  return result;
}

Roe<void> MockBlobClient::FileDelete(const std::string& /*relay_user_id*/, const std::string& blob_id) {
  deleted_file_ids_.push_back(blob_id);
  return Roe<void>{};
}

Roe<RelayFileListResult> MockBlobClient::FileList(const std::string& /*relay_user_id*/,
                                                  const std::string& status_filter) {
  file_list_filters_.push_back(status_filter);
  RelayFileListResult result = file_list_result_.value_or(RelayFileListResult{});
  if (!status_filter.empty()) {
    std::erase_if(result.files, [&](const RelayFileRecord& file) { return file.status != status_filter; });
  }
  return result;
}

Roe<std::vector<mesh::DirectoryHit>> MockDirectoryClient::SearchPeople(const std::string& query) {
  mesh::DirectoryHit alice;
  alice.hit_id = "hit_alice";
  alice.display_name = "Alice Example";
  alice.nickname = "alice";
  alice.account_id = "account:alice_acct";
  alice.ids = {{mesh::ContactIdKind::Account, "account:alice_acct", true},
               {mesh::ContactIdKind::RelayUser, "relay:alice123", false}};
  alice.signing_public_key_b64 = "A6EHv/POEL4dcN0Y50vAmWfk1jCbpQ1fHdyGZBJVMbg=";
  if (auto kem = MockPeerKemPublicKeyB64()) {
    alice.kem_public_key_b64 = *kem;
  } else if (!default_kem_public_key_b64_.empty()) {
    alice.kem_public_key_b64 = default_kem_public_key_b64_;
  }

  mesh::DirectoryHit bob;
  bob.hit_id = "hit_bob";
  bob.display_name = "Bob Builder";
  bob.nickname = "bob";
  bob.account_id = "account:bob_acct";
  bob.ids = {{mesh::ContactIdKind::Account, "account:bob_acct", true},
             {mesh::ContactIdKind::RelayUser, "relay:bob456", false}};
  bob.signing_public_key_b64 = "A6EHv/POEL4dcN0Y50vAmWfk1jCbpQ1fHdyGZBJVMbg=";
  if (auto kem = MockPeerKemPublicKeyB64()) {
    bob.kem_public_key_b64 = *kem;
  } else if (!default_kem_public_key_b64_.empty()) {
    bob.kem_public_key_b64 = default_kem_public_key_b64_;
  }

  if (query.empty()) {
    return std::vector<mesh::DirectoryHit>{alice, bob};
  }

  std::vector<mesh::DirectoryHit> out;
  for (const mesh::DirectoryHit& hit : {alice, bob}) {
    if (hit.display_name.find(query) != std::string::npos || hit.nickname.find(query) != std::string::npos) {
      out.push_back(hit);
      continue;
    }
    if (hit.account_id && hit.account_id->find(query) != std::string::npos) {
      out.push_back(hit);
      continue;
    }
    for (const mesh::ContactId& id : hit.ids) {
      if (id.value.find(query) != std::string::npos) {
        out.push_back(hit);
        break;
      }
    }
  }
  return out;
}

Roe<mesh::DirectoryHit> MockDirectoryClient::LookupRelayUser(const std::string& relay_user_id) {
  auto hits = SearchPeople("");
  if (!hits) {
    return hits.error();
  }
  for (const mesh::DirectoryHit& hit : *hits) {
    for (const mesh::ContactId& id : hit.ids) {
      if (id.kind == mesh::ContactIdKind::RelayUser && id.value == relay_user_id) {
        return hit;
      }
    }
  }
  return Error("Relay user not found");
}

Roe<mesh::DirectoryHit> MockDirectoryClient::LookupByAccount(const std::string& account_id) {
  auto hits = SearchPeople("");
  if (!hits) {
    return hits.error();
  }
  for (const mesh::DirectoryHit& hit : *hits) {
    if (hit.account_id && *hit.account_id == account_id) {
      return hit;
    }
    for (const mesh::ContactId& id : hit.ids) {
      if (id.kind == mesh::ContactIdKind::Account && id.value == account_id) {
        return hit;
      }
    }
  }
  return Error("Account not found");
}


Roe<RegistrationStartResult> MockRegistrationClient::StartRegistration(const std::string& /*public_key_b64*/,
                                                                       const std::string& /*nickname*/,
                                                                       const std::string& signature_alg,
                                                                       const std::string& /*kem_public_key_b64*/,
                                                                       const std::string& /*peer_id*/,
                                                                       const std::vector<std::string>& /*multiaddrs*/,
                                                                       const RegistrationPublishOpts& /*publish*/) {
  return RegistrationStartResult{.challenge = "mock-challenge", .signature_alg = signature_alg,
                                 .expires_at = "2099-01-01T00:00:00.000Z"};
}

Roe<RegistrationResult> MockRegistrationClient::FinishRegistration(const std::string& /*challenge*/,
                                                                   const std::string& public_key_b64,
                                                                   const std::string& nickname,
                                                                   const std::string& /*signature*/,
                                                                   int64_t /*timestamp*/,
                                                                   const std::string& /*signature_alg*/,
                                                                   const std::string& /*kem_public_key_b64*/,
                                                                   const std::string& /*kem_binding_signature_b64*/,
                                                                   const std::string& /*peer_id*/,
                                                                   const std::vector<std::string>& /*multiaddrs*/,
                                                                   int64_t initiation_floor,
                                                                   const RegistrationPublishOpts& /*publish*/) {
  return RegistrationResult{.success = true,
                            .relay_user_id = "relay:" + public_key_b64.substr(0, 12),
                            .message = "Registered as " + nickname,
                            .llm_api_key = "brf_llm_mock_key",
                            .expires_at = "2099-01-01T00:00:00.000Z",
                            .initiation_floor = initiation_floor,
                            .initiation_floor_present = true,
                            .kem_bound = true};
}

Roe<RegistrationResult> MockRegistrationClient::UpdateNickname(const std::string& new_nickname,
                                                                 const std::string& /*signature*/,
                                                                 int64_t /*timestamp*/,
                                                                 const std::string& /*relay_user_id*/) {
  return RegistrationResult{.success = true, .message = "Nickname updated to " + new_nickname};
}

Roe<void> MockRegistrationClient::RevokeRegistration(const std::string& /*relay_user_id*/,
                                                     const std::string& /*signature*/, const int64_t /*timestamp*/) {
  return {};
}

Roe<ClientCompatDocument> MockClientCompatClient::Fetch() {
  if (!error_.empty()) {
    return Error(error_);
  }
  if (!has_document_) {
    return Error("Mock client-compat document not set");
  }
  return document_;
}
} // namespace pbr
