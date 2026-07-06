#pragma once

#include "base/messaging/IThreadStore.h"
#include "base/messaging/ThreadTypes.h"
#include "base/people/IdentityStore.h"

#include "common/Error.h"

namespace pbr {

/** D060 responder — serve signed outbound envelopes from local thread.db. */
class ChatHistoryResponder {
public:
  static Roe<ChatHistoryResponse> Serve(IThreadStore& store, IdentityStore& identity,
                                        const ChatHistoryRequest& request,
                                        const std::string& local_relay_user_id);
};

} // namespace pbr
