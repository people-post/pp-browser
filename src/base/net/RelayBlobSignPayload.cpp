#include "base/net/RelayBlobSignPayload.h"

#include "base/net/RelaySignBytes.h"

namespace pbr {

namespace {

constexpr const char kBlobPresignDomain[] = "pp-browser:relay-blob-presign-v1";
constexpr const char kBlobRetainDomain[] = "pp-browser:relay-blob-retain-v1";
constexpr const char kBlobDeleteDomain[] = "pp-browser:relay-blob-delete-v1";
constexpr const char kBlobListDomain[] = "pp-browser:relay-blob-list-v1";
constexpr const char kProfileIconDomain[] = "pp-browser:relay-profile-icon-v1";
constexpr uint8_t kBlobSignVersion = 1;

std::vector<uint8_t> BuildDomainSignBytes(const char* domain, const auto& append_fields) {
  std::ostringstream oss;
  RelaySignAppendDomain(oss, domain);
  RelaySignAppendU8(oss, kBlobSignVersion);
  append_fields(oss);
  return RelaySignOssToBytes(oss);
}

} // namespace

std::vector<uint8_t> BuildBlobPresignSignBytes(const std::string& relay_user_id, const std::string& content_type,
                                               const uint64_t byte_length, const std::string& purpose,
                                               const int64_t timestamp) {
  return BuildDomainSignBytes(kBlobPresignDomain, [&](std::ostringstream& oss) {
    RelaySignAppendWireLenUtf8(oss, relay_user_id);
    RelaySignAppendWireLenUtf8(oss, content_type);
    RelaySignAppendU64(oss, byte_length);
    RelaySignAppendWireLenUtf8(oss, purpose);
    RelaySignAppendI64(oss, timestamp);
  });
}

std::vector<uint8_t> BuildBlobRetainSignBytes(const std::string& relay_user_id, const std::string& blob_id,
                                              const int64_t timestamp) {
  return BuildDomainSignBytes(kBlobRetainDomain, [&](std::ostringstream& oss) {
    RelaySignAppendWireLenUtf8(oss, relay_user_id);
    RelaySignAppendWireLenUtf8(oss, blob_id);
    RelaySignAppendI64(oss, timestamp);
  });
}

std::vector<uint8_t> BuildBlobDeleteSignBytes(const std::string& relay_user_id, const std::string& blob_id,
                                              const int64_t timestamp) {
  return BuildDomainSignBytes(kBlobDeleteDomain, [&](std::ostringstream& oss) {
    RelaySignAppendWireLenUtf8(oss, relay_user_id);
    RelaySignAppendWireLenUtf8(oss, blob_id);
    RelaySignAppendI64(oss, timestamp);
  });
}

std::vector<uint8_t> BuildBlobListSignBytes(const std::string& relay_user_id, const std::string& status_filter,
                                            const int64_t timestamp) {
  return BuildDomainSignBytes(kBlobListDomain, [&](std::ostringstream& oss) {
    RelaySignAppendWireLenUtf8(oss, relay_user_id);
    RelaySignAppendWireLenUtf8(oss, status_filter);
    RelaySignAppendI64(oss, timestamp);
  });
}

std::vector<uint8_t> BuildProfileIconSignBytes(const std::string& relay_user_id, const std::string& url,
                                               const std::string& blob_id, const std::string& kind,
                                               const int64_t timestamp) {
  return BuildDomainSignBytes(kProfileIconDomain, [&](std::ostringstream& oss) {
    RelaySignAppendWireLenUtf8(oss, relay_user_id);
    RelaySignAppendWireLenUtf8(oss, url);
    RelaySignAppendWireLenUtf8(oss, blob_id);
    RelaySignAppendWireLenUtf8(oss, kind);
    RelaySignAppendI64(oss, timestamp);
  });
}

} // namespace pbr
