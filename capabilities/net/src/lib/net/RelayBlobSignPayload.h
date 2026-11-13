#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace pbr {

std::vector<uint8_t> BuildBlobPresignSignBytes(const std::string& account_id, const std::string& content_type,
                                               uint64_t byte_length, const std::string& purpose, int64_t timestamp);

std::vector<uint8_t> BuildBlobRetainSignBytes(const std::string& account_id, const std::string& blob_id,
                                              int64_t timestamp);

std::vector<uint8_t> BuildBlobDeleteSignBytes(const std::string& account_id, const std::string& blob_id,
                                              int64_t timestamp);

std::vector<uint8_t> BuildBlobListSignBytes(const std::string& account_id, const std::string& status_filter,
                                            int64_t timestamp);

std::vector<uint8_t> BuildProfileIconSignBytes(const std::string& account_id, const std::string& url,
                                               const std::string& blob_id, const std::string& kind, int64_t timestamp);

// Relay files (`/files/*`, encrypted attachments): `pp-browser:relay-file-<op>-v1`, fields in the order of
// app-static-api docs/relay-files.md.
std::vector<uint8_t> BuildFileStartSignBytes(const std::string& account_id, uint64_t byte_length, int64_t timestamp);

/** One multipart part's PUT URL (`/files/part`); `part_number` counts from 1. */
std::vector<uint8_t> BuildFilePartSignBytes(const std::string& account_id, const std::string& blob_id,
                                            uint64_t part_number, int64_t timestamp);

std::vector<uint8_t> BuildFileCompleteSignBytes(const std::string& account_id, const std::string& blob_id,
                                                int64_t timestamp);

std::vector<uint8_t> BuildFileDeleteSignBytes(const std::string& account_id, const std::string& blob_id,
                                              int64_t timestamp);

/** `status_filter` "" (all), "pending" or "complete". */
std::vector<uint8_t> BuildFileListSignBytes(const std::string& account_id, const std::string& status_filter,
                                            int64_t timestamp);

} // namespace pbr
