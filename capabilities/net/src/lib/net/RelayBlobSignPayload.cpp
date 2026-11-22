#include "lib/net/RelayBlobSignPayload.h"

#include "lib/net/RelaySignBytes.h"

namespace pbr {

namespace {

/** The commonest layout: `account_id`, one more string, `timestamp`. */
std::vector<uint8_t> BuildAccountTextSignBytes(const char* domain, const std::string& audience,
                                               const std::string& account_id, const std::string& text,
                                               const int64_t timestamp, const pp::ExtArea& ext) {
  return BuildRelayCallBytes(domain, audience, ext,
                             [&](SignedBytesWriter& out) { out.Utf8(account_id).Utf8(text).I64(timestamp); });
}

} // namespace

std::vector<uint8_t> BuildBlobPresignSignBytes(const std::string& audience, const std::string& account_id,
                                               const std::string& content_type, const uint64_t byte_length,
                                               const std::string& purpose, const int64_t timestamp,
                                               const pp::ExtArea& ext) {
  return BuildRelayCallBytes(kRelayBlobPresignDomain, audience, ext, [&](SignedBytesWriter& out) {
    out.Utf8(account_id).Utf8(content_type).U64(byte_length).Utf8(purpose).I64(timestamp);
  });
}

std::vector<uint8_t> BuildBlobRetainSignBytes(const std::string& audience, const std::string& account_id,
                                              const std::string& blob_id, const int64_t timestamp,
                                              const pp::ExtArea& ext) {
  return BuildAccountTextSignBytes(kRelayBlobRetainDomain, audience, account_id, blob_id, timestamp, ext);
}

std::vector<uint8_t> BuildBlobDeleteSignBytes(const std::string& audience, const std::string& account_id,
                                              const std::string& blob_id, const int64_t timestamp,
                                              const pp::ExtArea& ext) {
  return BuildAccountTextSignBytes(kRelayBlobDeleteDomain, audience, account_id, blob_id, timestamp, ext);
}

std::vector<uint8_t> BuildProfileIconSignBytes(const std::string& audience, const std::string& account_id,
                                               const std::string& url, const std::string& blob_id,
                                               const std::string& kind, const int64_t timestamp,
                                               const pp::ExtArea& ext) {
  return BuildRelayCallBytes(kRelayProfileIconDomain, audience, ext, [&](SignedBytesWriter& out) {
    out.Utf8(account_id).Utf8(url).Utf8(blob_id).Utf8(kind).I64(timestamp);
  });
}

std::vector<uint8_t> BuildFileStartSignBytes(const std::string& audience, const std::string& account_id,
                                             const uint64_t byte_length, const int64_t timestamp,
                                             const pp::ExtArea& ext) {
  return BuildRelayCallBytes(kRelayFileStartDomain, audience, ext,
                             [&](SignedBytesWriter& out) { out.Utf8(account_id).U64(byte_length).I64(timestamp); });
}

std::vector<uint8_t> BuildFilePartSignBytes(const std::string& audience, const std::string& account_id,
                                            const std::string& blob_id, const uint64_t part_number,
                                            const int64_t timestamp, const pp::ExtArea& ext) {
  return BuildRelayCallBytes(kRelayFilePartDomain, audience, ext, [&](SignedBytesWriter& out) {
    out.Utf8(account_id).Utf8(blob_id).U64(part_number).I64(timestamp);
  });
}

std::vector<uint8_t> BuildFileStatusSignBytes(const std::string& audience, const std::string& account_id,
                                              const std::string& blob_id, const int64_t timestamp,
                                              const pp::ExtArea& ext) {
  return BuildAccountTextSignBytes(kRelayFileStatusDomain, audience, account_id, blob_id, timestamp, ext);
}

std::vector<uint8_t> BuildFileCompleteSignBytes(const std::string& audience, const std::string& account_id,
                                                const std::string& blob_id, const int64_t timestamp,
                                                const pp::ExtArea& ext) {
  return BuildAccountTextSignBytes(kRelayFileCompleteDomain, audience, account_id, blob_id, timestamp, ext);
}

std::vector<uint8_t> BuildFileDeleteSignBytes(const std::string& audience, const std::string& account_id,
                                              const std::string& blob_id, const int64_t timestamp,
                                              const pp::ExtArea& ext) {
  return BuildAccountTextSignBytes(kRelayFileDeleteDomain, audience, account_id, blob_id, timestamp, ext);
}

std::vector<uint8_t> BuildFileListSignBytes(const std::string& audience, const std::string& account_id,
                                            const std::string& status_filter, const int64_t timestamp,
                                            const pp::ExtArea& ext) {
  return BuildAccountTextSignBytes(kRelayFileListDomain, audience, account_id, status_filter, timestamp, ext);
}

} // namespace pbr
