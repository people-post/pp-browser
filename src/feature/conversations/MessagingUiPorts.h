#pragma once

#include "feature/conversations/ConversationsHub.h"

namespace pbr {

// ProjectMessagingView(ConversationsHub&) forces including ConversationsHub.h here; that
// is fine for the projection helper, but UI surfaces should prefer
// ConversationsFacade::Snapshot() rather than depending on ConversationsHub directly.

/** Read-only messaging fields UI surfaces may bind without holding ConversationsHub*. */
struct MessagingView {
  bool initialized = false;
  bool messaging_ready = false;
  bool has_router = false;
  std::string active_thread_id;
};

inline MessagingView ProjectMessagingView(ConversationsHub& hub) {
  MessagingView view;
  view.initialized = hub.IsInitialized();
  view.messaging_ready = hub.IsMessagingReady();
  view.has_router = hub.HasRouter();
  if (view.initialized) {
    view.active_thread_id = hub.Inbox().ActiveThreadId();
  }
  return view;
}

/**
 * Messaging read snapshot for UI presenters. Application fills from ConversationsHub.
 * Imperative ops live on ConversationsFacade (see ConversationsFacade.h).
 */
struct MessagingUiPorts {
  std::function<MessagingView()> snapshot;
};

} // namespace pbr
