#pragma once

#include <cstdint>
#include <string>

namespace pbr {

enum class AttachmentDownloadPolicy { Smart, AlwaysAuto, OnDemand };

std::string AttachmentDownloadPolicyToString(AttachmentDownloadPolicy policy);
AttachmentDownloadPolicy AttachmentDownloadPolicyFromString(const std::string& value);

/**
 * R021 / R008 — whether ingest / thread-open should auto-queue CDN fetch. `smart_max_bytes` is the largest
 * size the Smart policy starts by itself (AttachmentLimits::auto_download_max_bytes; common cannot include lib).
 */
bool ShouldAutoEnqueueAttachment(AttachmentDownloadPolicy policy, uint64_t byte_length, bool backlog_drain,
                                 uint64_t smart_max_bytes);

} // namespace pbr
