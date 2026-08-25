#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace pbr {

std::vector<uint8_t> BuildBlobPresignSignBytes(const std::string& relay_user_id, const std::string& content_type,
                                               uint64_t byte_length, const std::string& purpose, int64_t timestamp);

std::vector<uint8_t> BuildBlobRetainSignBytes(const std::string& relay_user_id, const std::string& blob_id,
                                              int64_t timestamp);

std::vector<uint8_t> BuildBlobDeleteSignBytes(const std::string& relay_user_id, const std::string& blob_id,
                                              int64_t timestamp);

std::vector<uint8_t> BuildBlobListSignBytes(const std::string& relay_user_id, const std::string& status_filter,
                                            int64_t timestamp);

std::vector<uint8_t> BuildProfileIconSignBytes(const std::string& relay_user_id, const std::string& url,
                                               const std::string& blob_id, const std::string& kind,
                                               int64_t timestamp);

} // namespace pbr
