#pragma once

#include "base/messaging/ChatPayloadTypes.h"
#include "base/crypto/CryptoTypes.h"

#include "common/Error.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace pbr {

/** Root `{profile}/threads/{thread_id}/blobs` (R016 / D075). */
std::string AttachmentBlobRoot(const std::string& profile_dir, const std::string& thread_id);

/** Session plaintext view root `{profile}/threads/{thread_id}/blobs_view` (a5 DEK-wrap). */
std::string AttachmentViewRoot(const std::string& profile_dir, const std::string& thread_id);

/** Lowercase hex of attachment plaintext hash. */
std::string AttachmentHashHex(const std::vector<uint8_t>& content_hash);

/** AAD: `attachment-blob|{profile_id}|{thread_id}|{hash_hex}|1`. */
std::string BuildAttachmentBlobAad(std::string_view profile_id, std::string_view thread_id,
                                   std::string_view hash_hex);

std::string AttachmentExtensionFromMime(const std::string& mime, const std::string& filename = {});

bool IsAttachmentImageMime(const std::string& mime);
bool IsAttachmentVideoMime(const std::string& mime);
bool AttachmentOpenNeedsConfirm(const std::string& mime);

std::string FormatAttachmentByteSize(uint64_t byte_length);

/** True when any on-disk blob (wrapped or legacy plaintext) exists for the hash. */
bool AttachmentBlobExists(const std::string& profile_dir, const std::string& thread_id,
                          const std::vector<uint8_t>& content_hash);

/**
 * Absolute path to a plaintext file for display/open: prefer `blobs_view/`, then legacy
 * plaintext under `blobs/` (never ciphertext / PPBA-wrapped files). Empty if unavailable.
 */
std::string AttachmentLocalPath(const std::string& profile_dir, const std::string& thread_id,
                                const std::vector<uint8_t>& content_hash, const std::string& mime,
                                const std::string& filename = {});

/**
 * Persist attachment bytes under `blobs/`. When `dek` is set, writes PPBA + FileCipher blob;
 * when null, writes legacy plaintext (tests / unlocked-less fixtures).
 */
Roe<std::string> SaveAttachmentPlaintext(const std::string& profile_dir, const std::string& thread_id,
                                         const std::vector<uint8_t>& content_hash, const std::string& mime,
                                         const ByteVector& plaintext, const std::string& filename = {},
                                         const ByteVector* dek = nullptr, std::string_view profile_id = {});

/** Load plaintext from wrapped or legacy blob under `blobs/`. */
Roe<ByteVector> LoadAttachmentPlaintext(const std::string& profile_dir, const std::string& thread_id,
                                        const std::vector<uint8_t>& content_hash, const std::string& mime,
                                        const std::string& filename, const ByteVector* dek,
                                        std::string_view profile_id);

/**
 * Ensure a plaintext file under `blobs_view/` (or return legacy plaintext path).
 * Materializes from wrapped storage when DEK is available.
 */
Roe<std::string> EnsureAttachmentViewPath(const std::string& profile_dir, const std::string& thread_id,
                                          const std::vector<uint8_t>& content_hash, const std::string& mime,
                                          const std::string& filename, const ByteVector* dek,
                                          std::string_view profile_id);

Roe<void> CopyAttachmentPlaintextFile(const std::string& profile_dir, const std::string& thread_id,
                                      const ChatAttachmentFields& fields, const std::string& source_path,
                                      const ByteVector* dek = nullptr, std::string_view profile_id = {});

/** Remove cached blobs + view materializations for a thread (R020 clear-history). */
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

/** Total bytes under all thread `blobs/`, `blobs_view/`, and `blob_cipher/` trees. */
uint64_t AttachmentCacheByteSize(const std::string& profile_dir);

/** Wipe downloaded attachment bytes for every thread under `{profile}/threads/`. */
Roe<void> WipeAllAttachmentCaches(const std::string& profile_dir);

} // namespace pbr
