#pragma once

#include <cstdint>
#include <string>

namespace pbr {

enum class AttachmentDownloadPolicy { Smart, AlwaysAuto, OnDemand };

std::string AttachmentDownloadPolicyToString(AttachmentDownloadPolicy policy);
AttachmentDownloadPolicy AttachmentDownloadPolicyFromString(const std::string& value);

/** R021 / R008 — whether ingest / thread-open should auto-queue CDN fetch. */
bool ShouldAutoEnqueueAttachment(AttachmentDownloadPolicy policy, uint64_t byte_length, bool backlog_drain);

} // namespace pbr
