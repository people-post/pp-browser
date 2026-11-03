#pragma once

#include "lib/content/CasStore.h"
#include "foundation/crypto/CryptoTypes.h"
#include "common/Error.h"
#include "common/thread/ThreadTypes.h"

#include <string>
#include <string_view>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * Chat attachments in the profile's private content store (lib/content), with the attachment
 * RAM cache in front (recent plaintext for views and peer fetches). Views and posters are files under
 * the thread's folder in `profile_dir`.
 */
bool AttachmentBlobExists(const CasStore& content, const std::vector<uint8_t>& content_hash);

/** Store an attachment's plaintext (its hash must be `content_hash`); returns the block path. */
Roe<std::string> SaveAttachmentPlaintext(CasStore& content, const std::vector<uint8_t>& content_hash,
                                         const std::string& mime, const ByteVector& plaintext,
                                         const std::string& filename, const ByteVector& dek);

Roe<ByteVector> LoadAttachmentPlaintext(const CasStore& content, const std::vector<uint8_t>& content_hash,
                                        const ByteVector& dek);

/** A plaintext view file under the thread's `blobs_view/`, made from the content store when missing. */
Roe<std::string> EnsureAttachmentView(const std::string& profile_dir, const CasStore& content,
                                      const std::string& thread_id, const std::vector<uint8_t>& content_hash,
                                      const std::string& mime, const std::string& filename, const ByteVector& dek);

/** The video poster beside the view, made from the content store's plaintext when missing. */
Roe<std::string> EnsureAttachmentPosterFromContent(const std::string& profile_dir, const CasStore& content,
                                                   const std::string& thread_id, const std::vector<uint8_t>& content_hash,
                                                   const std::string& mime, const std::string& filename,
                                                   const ByteVector& dek, uint64_t known_byte_length,
                                                   uint64_t max_receive_bytes);

/**
 * The low-res thumbnail of a stored image (kept at the poster path, wiped with the view cache), made once on the
 * caller's worker: the image is decrypted into a temporary file, thumbnailed by the platform hook and the
 * temporary file deleted. Fails when no hook is installed or the picture cannot be thumbnailed.
 */
Roe<std::string> EnsureAttachmentImageThumbnailFromContent(const std::string& profile_dir, const CasStore& content,
                                                           const std::string& thread_id,
                                                           const std::vector<uint8_t>& content_hash,
                                                           const std::string& mime, const std::string& filename,
                                                           const ByteVector& dek);

/**
 * A file we sent: its plaintext (exactly the bytes that were encrypted and uploaded) into the content store,
 * and its view when it shows inline (`max_receive_bytes`: AttachmentLimits).
 */
Roe<void> CopyAttachmentPlaintext(const std::string& profile_dir, CasStore& content,
                                  const std::string& thread_id, const ChatAttachmentFields& fields,
                                  const ByteVector& plaintext, const ByteVector& dek, uint64_t max_receive_bytes);

} // namespace pbr
