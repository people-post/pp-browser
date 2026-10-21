#pragma once

#include "foundation/crypto/CryptoTypes.h"
#include "common/Error.h"
#include "common/thread/ThreadTypes.h"

#include <string>
#include <string_view>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * Chat attachments in the profile's private content store (domain/content), with the attachment
 * RAM cache in front (recent plaintext for views and peer fetches).
 */
bool AttachmentBlobExists(const std::string& profile_dir, const std::vector<uint8_t>& content_hash);

/** Store an attachment's plaintext (its hash must be `content_hash`); returns the block path. */
Roe<std::string> SaveAttachmentPlaintext(const std::string& profile_dir, std::string_view profile_id,
                                         const std::vector<uint8_t>& content_hash, const std::string& mime,
                                         const ByteVector& plaintext, const std::string& filename,
                                         const ByteVector& dek);

Roe<ByteVector> LoadAttachmentPlaintext(const std::string& profile_dir, std::string_view profile_id,
                                        const std::vector<uint8_t>& content_hash, const ByteVector& dek);

/** A plaintext view file under the thread's `blobs_view/`, made from the content store when missing. */
Roe<std::string> EnsureAttachmentView(const std::string& profile_dir, std::string_view profile_id,
                                      const std::string& thread_id, const std::vector<uint8_t>& content_hash,
                                      const std::string& mime, const std::string& filename, const ByteVector& dek);

/** The video poster beside the view, made from the content store's plaintext when missing. */
Roe<std::string> EnsureAttachmentPosterFromContent(const std::string& profile_dir, std::string_view profile_id,
                                                   const std::string& thread_id, const std::vector<uint8_t>& content_hash,
                                                   const std::string& mime, const std::string& filename,
                                                   const ByteVector& dek, uint64_t known_byte_length = 0);

/** A file we sent: its plaintext into the content store, and its view when it shows inline. */
Roe<void> CopyAttachmentPlaintextFile(const std::string& profile_dir, std::string_view profile_id,
                                      const std::string& thread_id, const ChatAttachmentFields& fields,
                                      const std::string& source_path, const ByteVector& dek);

} // namespace pbr
