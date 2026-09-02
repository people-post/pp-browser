#pragma once

#include "base/crypto/IPskSessionStore.h"
#include "common/thread/IThreadStore.h"
#include "common/thread/ThreadTypes.h"
#include "base/people/IdentityStore.h"

#include "common/Error.h"
#include "common/PbrCompat.h"

namespace pbr {

/** D060 responder — serve signed outbound envelopes from local thread.db. */
class ChatHistoryResponder {
public:
  static Roe<ChatHistoryResponse> Serve(IThreadStore& store, IdentityStore& identity, IPskSessionStore& psk_store,
                                        const ChatHistoryRequest& request, const std::string& local_relay_user_id);
};

} // namespace pbr
