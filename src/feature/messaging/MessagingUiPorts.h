#pragma once

#include "feature/messaging/MessagingHub.h"

namespace pbr {

/** Read-only messaging fields UI surfaces may bind without holding MessagingHub*. */
struct MessagingView {
  bool initialized = false;
  bool messaging_ready = false;
  bool has_router = false;
  std::string active_thread_id;
};

inline MessagingView ProjectMessagingView(const MessagingHub& hub) {
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
 * Imperative ops remain on hub until MessagingFacade (Phase 6).
 */
struct MessagingUiPorts {
  std::function<MessagingView()> snapshot;
};

} // namespace pbr
