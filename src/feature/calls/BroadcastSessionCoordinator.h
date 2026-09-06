#pragma once

#include "domain/messaging/AnnounceLiveJoin.h"
#include "domain/messaging/CallTypes.h"
#include "common/Error.h"
#include "common/Module.h"

#include <functional>
#include <optional>
#include <string>

#include "common/PbrCompat.h"

namespace pbr {

class CallSessionStore;
class ContactsStore;
class CallMediaKeyStore;

/**
 * Broadcast (live-announce) join arm/accept — separated from 1:1 / group SoftMigrate paths.
 * CallSessionManager owns the instance and supplies host ports; ConversationsFacade / CallUiBackend
 * should prefer this surface for Spine C watch joins.
 */
class BroadcastSessionCoordinator : public Module {
public:
  struct HostPorts {
    std::function<Roe<std::string>()> local_relay_identity;
    /** Optional; may return empty when mesh PeerId unknown. */
    std::function<std::string()> local_mesh_peer_id;
    std::function<void()> notify_ring_changed;
    std::function<void()> sweep_expired_invites;
    std::function<Roe<void>(const std::string& keep_call_id)> leave_call_if_active_except;
    std::function<bool(const std::string& call_id, const std::optional<std::string>& sfu_hint)>
        on_announce_viewer_joined;
  };

  BroadcastSessionCoordinator(CallSessionStore& sessions, ContactsStore& contacts,
                              CallMediaKeyStore& media_keys);

  void SetHostPorts(HostPorts ports);

  /**
   * Arm pending invite + ringing Broadcast session from a live-join plan.
   * Does not SoftMigrate or attach media.
   */
  Roe<PendingCallInvite> ArmJoinFromLiveAnnounce(const AnnounceLiveJoinPlan& plan,
                                                 const ArmLiveAnnounceJoinOpts& opts = {});

  /**
   * Accept an armed live-announce invite without SoftMigrate or 1:1 media.
   * Attaches SFU when session/pending carries sfu_hint; otherwise marks joined and defers media.
   */
  Roe<void> AcceptLiveAnnounceJoin(const std::string& call_id);

private:
  CallSessionStore& sessions_;
  ContactsStore& contacts_;
  CallMediaKeyStore& media_keys_;
  HostPorts host_;
};

} // namespace pbr
