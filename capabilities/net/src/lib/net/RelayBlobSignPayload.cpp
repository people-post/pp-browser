#include "lib/net/RelayBlobSignPayload.h"

#include "lib/net/RelaySignBytes.h"

namespace pbr {

namespace {

constexpr const char kBlobPresignDomain[] = "pp-browser:relay-blob-presign-v1";
constexpr const char kBlobRetainDomain[] = "pp-browser:relay-blob-retain-v1";
constexpr const char kBlobDeleteDomain[] = "pp-browser:relay-blob-delete-v1";
constexpr const char kBlobListDomain[] = "pp-browser:relay-blob-list-v1";
constexpr const char kProfileIconDomain[] = "pp-browser:relay-profile-icon-v1";
constexpr const char kFileStartDomain[] = "pp-browser:relay-file-start-v1";
constexpr const char kFilePartDomain[] = "pp-browser:relay-file-part-v1";
constexpr const char kFileStatusDomain[] = "pp-browser:relay-file-status-v1";
constexpr const char kFileCompleteDomain[] = "pp-browser:relay-file-complete-v1";
constexpr const char kFileDeleteDomain[] = "pp-browser:relay-file-delete-v1";
constexpr const char kFileListDomain[] = "pp-browser:relay-file-list-v1";
// 2 (multi-relay MR007): the audience after the version.
constexpr uint8_t kBlobSignVersion = 2;

std::vector<uint8_t> BuildDomainSignBytes(const char* domain, const std::string& audience, const auto& append_fields) {
  std::ostringstream oss;
  if (!RelaySignAppendCommandHeader(oss, domain, kBlobSignVersion, audience)) {
    return {};
  }
  append_fields(oss);
  return RelaySignOssToBytes(oss);
}

/** The commonest layout: `account_id`, one more string, `timestamp`. */
std::vector<uint8_t> BuildAccountTextSignBytes(const char* domain, const std::string& audience,
                                               const std::string& account_id, const std::string& text,
                                               const int64_t timestamp) {
  return BuildDomainSignBytes(domain, audience, [&](std::ostringstream& oss) {
    RelaySignAppendWireLenUtf8(oss, account_id);
    RelaySignAppendWireLenUtf8(oss, text);
    RelaySignAppendI64(oss, timestamp);
  });
}

} // namespace

std::vector<uint8_t> BuildBlobPresignSignBytes(const std::string& audience, const std::string& account_id,
                                               const std::string& content_type, const uint64_t byte_length,
                                               const std::string& purpose, const int64_t timestamp) {
  return BuildDomainSignBytes(kBlobPresignDomain, audience, [&](std::ostringstream& oss) {
    RelaySignAppendWireLenUtf8(oss, account_id);
    RelaySignAppendWireLenUtf8(oss, content_type);
    RelaySignAppendU64(oss, byte_length);
    RelaySignAppendWireLenUtf8(oss, purpose);
    RelaySignAppendI64(oss, timestamp);
  });
}

std::vector<uint8_t> BuildBlobRetainSignBytes(const std::string& audience, const std::string& account_id,
                                              const std::string& blob_id, const int64_t timestamp) {
  return BuildAccountTextSignBytes(kBlobRetainDomain, audience, account_id, blob_id, timestamp);
}

std::vector<uint8_t> BuildBlobDeleteSignBytes(const std::string& audience, const std::string& account_id,
                                              const std::string& blob_id, const int64_t timestamp) {
  return BuildAccountTextSignBytes(kBlobDeleteDomain, audience, account_id, blob_id, timestamp);
}

std::vector<uint8_t> BuildBlobListSignBytes(const std::string& audience, const std::string& account_id,
                                            const std::string& status_filter, const int64_t timestamp) {
  return BuildAccountTextSignBytes(kBlobListDomain, audience, account_id, status_filter, timestamp);
}

std::vector<uint8_t> BuildProfileIconSignBytes(const std::string& audience, const std::string& account_id,
                                               const std::string& url, const std::string& blob_id,
                                               const std::string& kind, const int64_t timestamp) {
  return BuildDomainSignBytes(kProfileIconDomain, audience, [&](std::ostringstream& oss) {
    RelaySignAppendWireLenUtf8(oss, account_id);
    RelaySignAppendWireLenUtf8(oss, url);
    RelaySignAppendWireLenUtf8(oss, blob_id);
    RelaySignAppendWireLenUtf8(oss, kind);
    RelaySignAppendI64(oss, timestamp);
  });
}

std::vector<uint8_t> BuildFileStartSignBytes(const std::string& audience, const std::string& account_id,
                                             const uint64_t byte_length, const int64_t timestamp) {
  return BuildDomainSignBytes(kFileStartDomain, audience, [&](std::ostringstream& oss) {
    RelaySignAppendWireLenUtf8(oss, account_id);
    RelaySignAppendU64(oss, byte_length);
    RelaySignAppendI64(oss, timestamp);
  });
}

std::vector<uint8_t> BuildFilePartSignBytes(const std::string& audience, const std::string& account_id,
                                            const std::string& blob_id, const uint64_t part_number,
                                            const int64_t timestamp) {
  return BuildDomainSignBytes(kFilePartDomain, audience, [&](std::ostringstream& oss) {
    RelaySignAppendWireLenUtf8(oss, account_id);
    RelaySignAppendWireLenUtf8(oss, blob_id);
    RelaySignAppendU64(oss, part_number);
    RelaySignAppendI64(oss, timestamp);
  });
}

std::vector<uint8_t> BuildFileStatusSignBytes(const std::string& audience, const std::string& account_id,
                                              const std::string& blob_id, const int64_t timestamp) {
  return BuildAccountTextSignBytes(kFileStatusDomain, audience, account_id, blob_id, timestamp);
}

std::vector<uint8_t> BuildFileCompleteSignBytes(const std::string& audience, const std::string& account_id,
                                                const std::string& blob_id, const int64_t timestamp) {
  return BuildAccountTextSignBytes(kFileCompleteDomain, audience, account_id, blob_id, timestamp);
}

std::vector<uint8_t> BuildFileDeleteSignBytes(const std::string& audience, const std::string& account_id,
                                              const std::string& blob_id, const int64_t timestamp) {
  return BuildAccountTextSignBytes(kFileDeleteDomain, audience, account_id, blob_id, timestamp);
}

std::vector<uint8_t> BuildFileListSignBytes(const std::string& audience, const std::string& account_id,
                                            const std::string& status_filter, const int64_t timestamp) {
  return BuildAccountTextSignBytes(kFileListDomain, audience, account_id, status_filter, timestamp);
}

} // namespace pbr
