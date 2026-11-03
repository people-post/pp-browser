#pragma once

#include "common/NetworkChange.h"

#include <cstdint>
#include <string>

namespace pbr {

enum class AttachmentDownloadPolicy { Smart, AlwaysAuto, OnDemand };

std::string AttachmentDownloadPolicyToString(AttachmentDownloadPolicy policy);
AttachmentDownloadPolicy AttachmentDownloadPolicyFromString(const std::string& value);

/**
 * R021 / R008 — whether ingest / thread-open should auto-queue CDN fetch. `smart_max_bytes` is the largest
 * size the Smart policy starts by itself (AttachmentLimits::auto_download_max_bytes; common cannot include lib).
 * `network_allows_all_auto`: the current network is one the person ticked for "download everything"; on any
 * other network AlwaysAuto behaves as Smart.
 */
bool ShouldAutoEnqueueAttachment(AttachmentDownloadPolicy policy, uint64_t byte_length, bool backlog_drain,
                                 uint64_t smart_max_bytes, bool network_allows_all_auto);

/** AlwaysAuto with neither network ticked is Smart (it could never download more than Smart). */
AttachmentDownloadPolicy EffectiveAttachmentDownloadPolicy(AttachmentDownloadPolicy policy, bool auto_on_wifi,
                                                           bool auto_on_cellular);

/** The kind of network for automatic downloads. */
enum class AutoDownloadNetwork { Unknown, Wifi, Mobile };

/**
 * How a reported network counts: on a phone, Wi-Fi is a Wi-Fi link that is not expensive; cellular, any other
 * link (VPN shell, unrecognised interface) and anything expensive (hotspot, metered, low data mode) are the
 * mobile network; offline or an unknown link is Unknown (the caller keeps the last known kind). A desktop is
 * always Wi-Fi.
 */
AutoDownloadNetwork AutoDownloadNetworkOf(const NetworkState& state, bool mobile_host);

/** Whether "download everything" applies on this (known) network, from the two ticks. */
bool AutoDownloadAllowedOn(AutoDownloadNetwork network, bool auto_on_wifi, bool auto_on_cellular);

/**
 * A voice message by what its sender declared: mime `audio/ogg`, name `voice-<digits>.ogg`, 1..1,000,000 bytes.
 * The same test as the voice bubble (lib/voice `DecideVoiceBubble`, which this library cannot link).
 */
bool IsDeclaredVoiceMessage(const std::string& mime, const std::string& filename, uint64_t byte_length);

} // namespace pbr
