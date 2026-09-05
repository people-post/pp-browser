#pragma once

#include "common/chat/ChatPayloadTypes.h"
#include "foundation/crypto/CryptoTypes.h"

#include "common/Error.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/** Session plaintext view root `{profile}/threads/{thread_id}/blobs_view`. */
std::string AttachmentViewRoot(const std::string& profile_dir, const std::string& thread_id);

/** Lowercase hex of attachment plaintext hash. */
std::string AttachmentHashHex(const std::vector<uint8_t>& content_hash);

std::string AttachmentExtensionFromMime(const std::string& mime, const std::string& filename = {});

bool IsAttachmentImageMime(const std::string& mime);
bool IsAttachmentVideoMime(const std::string& mime);
bool AttachmentOpenNeedsConfirm(const std::string& mime);

std::string FormatAttachmentByteSize(uint64_t byte_length);

/** True when private CAS holds the hash. */
bool AttachmentBlobExists(const std::string& profile_dir, const std::string& thread_id,
                          const std::vector<uint8_t>& content_hash);

/**
 * Absolute path to a plaintext file for display/open under `blobs_view/` only.
 * Empty if the view is not materialized.
 */
std::string AttachmentLocalPath(const std::string& profile_dir, const std::string& thread_id,
                                const std::vector<uint8_t>& content_hash, const std::string& mime,
                                const std::string& filename = {});

/**
 * Persist attachment bytes into private CAS (PPBA under cas/private). Requires `dek` and
 * `profile_id` (C007 — no thread `blobs/` durable store).
 */
Roe<std::string> SaveAttachmentPlaintext(const std::string& profile_dir, const std::string& thread_id,
                                         const std::vector<uint8_t>& content_hash, const std::string& mime,
                                         const ByteVector& plaintext, const std::string& filename,
                                         const ByteVector& dek, std::string_view profile_id);

/** Load plaintext from private CAS. Requires `dek` and `profile_id`. */
Roe<ByteVector> LoadAttachmentPlaintext(const std::string& profile_dir, const std::string& thread_id,
                                        const std::vector<uint8_t>& content_hash, const std::string& mime,
                                        const std::string& filename, const ByteVector& dek,
                                        std::string_view profile_id);

/**
 * Ensure a plaintext file under `blobs_view/` by loading from private CAS when needed.
 */
Roe<std::string> EnsureAttachmentViewPath(const std::string& profile_dir, const std::string& thread_id,
                                          const std::vector<uint8_t>& content_hash, const std::string& mime,
                                          const std::string& filename, const ByteVector& dek,
                                          std::string_view profile_id);

/** `{blobs_view}/{hash}.poster.jpg` — session plaintext poster (wiped with view cache). */
std::string AttachmentPosterPath(const std::string& profile_dir, const std::string& thread_id,
                                 const std::vector<uint8_t>& content_hash);

bool AttachmentPosterExists(const std::string& profile_dir, const std::string& thread_id,
                            const std::vector<uint8_t>& content_hash);

/**
 * Ensure a JPEG poster under blobs_view for a video attachment (R012).
 * Best-effort extract; soft placeholder on decode failure.
 */
Roe<std::string> EnsureAttachmentPoster(const std::string& profile_dir, const std::string& thread_id,
                                        const std::vector<uint8_t>& content_hash, const std::string& mime,
                                        const std::string& filename, const ByteVector& dek,
                                        std::string_view profile_id);

Roe<void> CopyAttachmentPlaintextFile(const std::string& profile_dir, const std::string& thread_id,
                                      const ChatAttachmentFields& fields, const std::string& source_path,
                                      const ByteVector& dek, std::string_view profile_id);

/** Remove view / pending-cipher caches for a thread (and any leftover pre-cutover `blobs/` dir). */
Roe<void> WipeThreadAttachmentBlobs(const std::string& profile_dir, const std::string& thread_id);

/** Wipe all `blobs_view/` trees (ClearDek / vault lock). */
Roe<void> WipeAllAttachmentViewCaches(const std::string& profile_dir);

/** Pending peer-push ciphertext before envelope key arrives (a6). */
std::string AttachmentPendingCiphertextRoot(const std::string& profile_dir, const std::string& thread_id);
Roe<void> SavePendingAttachmentCiphertext(const std::string& profile_dir, const std::string& thread_id,
                                          const std::vector<uint8_t>& content_hash,
                                          const std::vector<uint8_t>& ciphertext);
bool AttachmentPendingCiphertextExists(const std::string& profile_dir, const std::string& thread_id,
                                       const std::vector<uint8_t>& content_hash);
Roe<ByteVector> LoadPendingAttachmentCiphertext(const std::string& profile_dir, const std::string& thread_id,
                                                const std::vector<uint8_t>& content_hash);
void RemovePendingAttachmentCiphertext(const std::string& profile_dir, const std::string& thread_id,
                                       const std::vector<uint8_t>& content_hash);

/** Total bytes under CAS private + thread `blobs_view/` / `blob_cipher/`. */
uint64_t AttachmentCacheByteSize(const std::string& profile_dir);

/** Wipe downloaded attachment bytes for every thread and clear private CAS. */
Roe<void> WipeAllAttachmentCaches(const std::string& profile_dir);

} // namespace pbr
