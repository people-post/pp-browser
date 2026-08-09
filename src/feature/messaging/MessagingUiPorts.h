#pragma once

#include "feature/messaging/MessagingHub.h"

namespace pbr {

// ProjectMessagingView(MessagingHub&) forces including MessagingHub.h here; that
// is fine for the projection helper, but UI surfaces should prefer
// MessagingFacade::Snapshot() rather than depending on MessagingHub directly.

/** Read-only messaging fields UI surfaces may bind without holding MessagingHub*. */
struct MessagingView {
  bool initialized = false;
  bool messaging_ready = false;
  bool has_router = false;
  std::string active_thread_id;
};

inline MessagingView ProjectMessagingView(MessagingHub& hub) {
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
 * Messaging read snapshot for UI presenters. Application fills from MessagingHub.
 * Imperative ops live on MessagingFacade (see MessagingFacade.h).
 */
struct MessagingUiPorts {
  std::function<MessagingView()> snapshot;
};

} // namespace pbr
