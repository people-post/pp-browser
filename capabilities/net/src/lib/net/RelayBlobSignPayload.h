#pragma once

#include "common/ExtArea.h"

#include <cstdint>
#include <string>
#include <vector>

namespace pbr {

// The storage function's signed calls (release-compatibility SIGNED_FORMATS § 2.8): the public CDN's `/v1/cdn/*`
// (domains `pp-browser:relay-blob-*`; it has no list), `/v1/profile/icon`, and the private blob store's `/v1/blob/*` (domains
// `pp-browser:relay-file-*`; the domain names predate the routes' and are frozen). Each is
// `domain ‖ 0x00 ‖ u8 4 ‖ LenUtf8(audience) ‖ fields ‖ ext area`, the fields in the order of
// docs/contracts/SERVICE_ENDPOINTS.md § The audience. Empty for an empty audience.

/** `/v1/cdn/presign`. */
std::vector<uint8_t> BuildBlobPresignSignBytes(const std::string& audience, const std::string& account_id,
                                               const std::string& content_type, uint64_t byte_length,
                                               const std::string& purpose, int64_t timestamp,
                                               const pp::ExtArea& ext = {});

/** `/v1/cdn/retain`. */
std::vector<uint8_t> BuildBlobRetainSignBytes(const std::string& audience, const std::string& account_id,
                                              const std::string& blob_id, int64_t timestamp,
                                              const pp::ExtArea& ext = {});

/** `/v1/cdn/delete`. */
std::vector<uint8_t> BuildBlobDeleteSignBytes(const std::string& audience, const std::string& account_id,
                                              const std::string& blob_id, int64_t timestamp,
                                              const pp::ExtArea& ext = {});

/** `/v1/profile/icon`. */
std::vector<uint8_t> BuildProfileIconSignBytes(const std::string& audience, const std::string& account_id,
                                               const std::string& url, const std::string& blob_id,
                                               const std::string& kind, int64_t timestamp, const pp::ExtArea& ext = {});

/** `/v1/blob/start`. */
std::vector<uint8_t> BuildFileStartSignBytes(const std::string& audience, const std::string& account_id,
                                             uint64_t byte_length, int64_t timestamp, const pp::ExtArea& ext = {});

/** `/v1/blob/part`: one multipart part's PUT URL; `part_number` counts from 1. */
std::vector<uint8_t> BuildFilePartSignBytes(const std::string& audience, const std::string& account_id,
                                            const std::string& blob_id, uint64_t part_number, int64_t timestamp,
                                            const pp::ExtArea& ext = {});

/** `/v1/blob/status` (the app does not ask it yet). */
std::vector<uint8_t> BuildFileStatusSignBytes(const std::string& audience, const std::string& account_id,
                                              const std::string& blob_id, int64_t timestamp,
                                              const pp::ExtArea& ext = {});

/** `/v1/blob/complete`. */
std::vector<uint8_t> BuildFileCompleteSignBytes(const std::string& audience, const std::string& account_id,
                                                const std::string& blob_id, int64_t timestamp,
                                                const pp::ExtArea& ext = {});

/** `/v1/blob/delete`. */
std::vector<uint8_t> BuildFileDeleteSignBytes(const std::string& audience, const std::string& account_id,
                                              const std::string& blob_id, int64_t timestamp,
                                              const pp::ExtArea& ext = {});

/** `/v1/blob/list`: `status_filter` "" (all), "pending" or "complete". */
std::vector<uint8_t> BuildFileListSignBytes(const std::string& audience, const std::string& account_id,
                                            const std::string& status_filter, int64_t timestamp,
                                            const pp::ExtArea& ext = {});

} // namespace pbr
