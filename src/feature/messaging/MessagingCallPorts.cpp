#include "feature/messaging/MessagingCallPorts.h"

#include "base/data/SessionStore.h"
#include "base/media/CallMediaHealth.h"
#include "feature/messaging/MessagingHub.h"

namespace pbr {

MessagingCallPorts MakeMessagingCallPorts(MessagingHub& hub, SessionStore* session_store) {
  MessagingCallPorts ports;
  ports.initialized = [&hub]() { return hub.IsInitialized(); };
  ports.calls = [&hub]() -> CallSessionManager* { return hub.Calls(); };
  ports.lifecycle = [&hub]() -> CallLifecycle* { return hub.Lifecycle(); };
  ports.get_thread = [&hub](const std::string& thread_id) { return hub.Store().GetThread(thread_id); };
  ports.find_contact_by_identity = [&hub](const std::string& identity, const ContactIdKind kind) {
    return hub.Contacts().FindByIdentity(identity, kind);
  };
  ports.local_relay_identity = [&hub]() -> std::optional<std::string> {
    if (auto identity = hub.Identity().Get()) {
      return identity->relay_user_id;
    }
    return std::nullopt;
  };
  ports.call_diagnostics_enabled = [session_store]() {
    const bool pref =
        session_store && session_store->IsInitialized() && session_store->Snapshot().profile_prefs.call_diagnostics;
    return CallDiagnosticsEnabled(pref);
  };
  return ports;
}

} // namespace pbr
