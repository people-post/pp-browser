#pragma once

#include "base/messaging/ThreadTypes.h"
#include "base/people/ContactsStore.h"
#include "base/people/IdentityStore.h"

#include "common/Error.h"

#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/** Build a peer-direct chat-blob request for a direct E2E thread (M010 relay routing). */
Roe<ChatBlobRequest> BuildChatBlobRequest(const Thread& thread, ContactsStore& contacts, IdentityStore& identity,
                                          ChatBlobOp op, const std::string& thread_id,
                                          const std::vector<uint8_t>& content_hash);

} // namespace pbr
