#pragma once

#include "common/chat/ChatPayloadTypes.h"
#include "common/Error.h"

#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

struct PreparedChatAttachment {
  ChatAttachmentFields fields;
  std::vector<uint8_t> ciphertext;
};

/** Encrypt file bytes without uploading. */
Roe<PreparedChatAttachment> PrepareChatAttachmentFromFile(const std::string& path);

} // namespace pbr
