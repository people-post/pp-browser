#pragma once

#include "feature/calls/CallHopPlanning.h"
#include "feature/calls/CallHopRanking.h"
#include "feature/calls/CallSessionEvents.h"
#include "feature/calls/CallHopRelayDeps.h"
#include "lib/calls/ICallSessionReader.h"
#include "lib/people/IContactBook.h"
#include "common/runtime/OwnerOutbox.h"

#include "common/PbrCompat.h"

namespace pbr {

/**
 * The device's side of the group (hop) path (V056, device level): the mesh's relay client and hop
 * discovery as calls see them (`RelayDeps`), the ranked hops this device could use, and hop planning
 * for a call's invitees and joiners (which runs before a call has media; its per-call state is
 * indexed by call id). Each call's hop path reads it. Calls owner.
 */
class CallHopService {
public:
  CallHopService(const ICallSessionReader& sessions, const IContactBook& contacts);
  CallHopService(const CallHopService&) = delete;
  CallHopService& operator=(const CallHopService&) = delete;

  /** The mesh's relay client and hop discovery (rebound on mesh start / stop; hop paths re-read it). */
  void SetMediaRelayDeps(CallHopMediaRelayDeps deps) { relay_deps_ = std::move(deps); }
  const CallHopMediaRelayDeps& RelayDeps() const { return relay_deps_; }

  CallHopRanking& Ranking() { return ranking_; }
  const CallHopRanking& Ranking() const { return ranking_; }
  CallHopPlanning& Planning() { return planning_; }

  /** Where planning reports its probe answers. */
  void SetOutbox(OwnerOutbox<PlanningEvent> outbox) { planning_.SetOutbox(std::move(outbox)); }
  void Handle(PlanningEvent& event) { planning_.Handle(event); }

private:
  CallHopMediaRelayDeps relay_deps_;
  CallHopRanking ranking_;
  CallHopPlanning planning_;
};

} // namespace pbr
