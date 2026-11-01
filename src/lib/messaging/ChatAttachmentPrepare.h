#pragma once

#include "common/chat/ChatPayloadTypes.h"
#include "common/Error.h"

#include <cstdint>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

struct PreparedChatAttachment {
  ChatAttachmentFields fields;
  std::vector<uint8_t> ciphertext;
};

/**
 * Encrypt attachment bytes without uploading. The caller has already checked the size against the
 * AttachmentLimits it uses; `max_bytes` is the backstop. `mime` empty: taken from the file name.
 */
Roe<PreparedChatAttachment> PrepareChatAttachmentFromBytes(const std::vector<uint8_t>& plaintext,
                                                           const std::string& filename, const std::string& mime,
                                                           uint64_t max_bytes);

/** Read a file (at most `max_bytes`) and encrypt it. */
Roe<PreparedChatAttachment> PrepareChatAttachmentFromFile(const std::string& path, uint64_t max_bytes);

} // namespace pbr
