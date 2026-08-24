#pragma once

#include "base/messaging/ChatPayloadTypes.h"
#include "base/crypto/CryptoTypes.h"

#include "common/Error.h"

#include <cstdint>
#include <string>
#include <vector>

namespace pbr {

/** Root `{profile}/threads/{thread_id}/blobs` (R016 / D075). */
std::string AttachmentBlobRoot(const std::string& profile_dir, const std::string& thread_id);

/** Lowercase hex of attachment plaintext hash. */
std::string AttachmentHashHex(const std::vector<uint8_t>& content_hash);

std::string AttachmentExtensionFromMime(const std::string& mime, const std::string& filename = {});

bool IsAttachmentImageMime(const std::string& mime);
bool IsAttachmentVideoMime(const std::string& mime);
bool AttachmentOpenNeedsConfirm(const std::string& mime);

std::string FormatAttachmentByteSize(uint64_t byte_length);

/** Absolute path when cached plaintext exists; otherwise empty. */
std::string AttachmentLocalPath(const std::string& profile_dir, const std::string& thread_id,
                                const std::vector<uint8_t>& content_hash, const std::string& mime,
                                const std::string& filename = {});

Roe<std::string> SaveAttachmentPlaintext(const std::string& profile_dir, const std::string& thread_id,
                                         const std::vector<uint8_t>& content_hash, const std::string& mime,
                                         const ByteVector& plaintext, const std::string& filename = {});

Roe<void> CopyAttachmentPlaintextFile(const std::string& profile_dir, const std::string& thread_id,
                                      const ChatAttachmentFields& fields, const std::string& source_path);

/** Remove cached plaintext blobs for a thread (R020 clear-history). */
Roe<void> WipeThreadAttachmentBlobs(const std::string& profile_dir, const std::string& thread_id);

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

} // namespace pbr
