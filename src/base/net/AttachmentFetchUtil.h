#pragma once

#include "base/messaging/ChatPayloadTypes.h"

#include "common/Error.h"

#include <vector>

namespace pbr {

/** GET CDN ciphertext, decrypt with attachment content key, verify plaintext hash. */
Roe<std::vector<uint8_t>> FetchAndDecryptAttachment(const ChatAttachmentFields& fields);

} // namespace pbr
