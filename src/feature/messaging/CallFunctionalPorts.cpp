#include "feature/messaging/CallFunctionalPorts.h"

#include "foundation/data/SessionStore.h"
#include "common/media/CallMediaHealth.h"
#include "feature/messaging/CallUiBackend.h"
#include "feature/messaging/MessagingHub.h"

namespace pbr {

CallFunctionalPorts MakeCallFunctionalPorts(CallUiBackend& backend, MessagingHub& hub,
                                            SessionStore* session_store) {
  CallFunctionalPorts ports;
  ports.initialized = [&hub]() { return hub.IsInitialized(); };
  ports.backend = [&backend]() -> CallUiBackend* { return &backend; };
  ports.get_thread = [&hub](const std::string& thread_id) { return hub.Store().GetThread(thread_id); };
  ports.find_contact_by_identity = [&hub](const std::string& identity, const ContactIdKind kind) {
    return hub.Contacts().FindByIdentity(identity, kind);
  };
  ports.identity_icon_local_path = [&hub](const std::string& identity) { return hub.IdentityIconLocalPath(identity); };
  ports.local_relay_identity = [&hub]() -> std::optional<std::string> {
    if (auto identity = hub.Identity().Get()) {
      if (!identity->account_id.empty()) {
        return identity->account_id;
      }
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
