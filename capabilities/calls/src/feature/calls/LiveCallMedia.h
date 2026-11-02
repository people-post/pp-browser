#pragma once

#include "lib/calls/LiveCall.h"
#include "feature/calls/CallMediaCoordinator.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>

namespace pbr {

/**
 * Each live call's media: its coordinator over the one engine, the seat and the path drivers. A
 * coordinator belongs to one admission of a call (LiveCall::Instance): a re-admitted id gets a fresh
 * one, and one whose call LiveCalls no longer keeps is dropped. Calls owner only.
 */
class LiveCallMedia {
public:
  explicit LiveCallMedia(const LiveCalls& live) : live_(live) {}

  /** The one media engine and the media seat every call's coordinator drives (seat may be null). */
  void BindMediaResources(MediaEngine* engine, CallMediaSeat* seat);
  /** How a call's 1:1 path is built (empty: none wired — no new paths; built ones stay until dropped). */
  void BindDirectPathFactory(CallDirectPathFactory make) { resources_.make_direct = std::move(make); }
  /** Drop every built 1:1 path (the transport they ride goes). */
  void DropDirectPaths();
  /** Each call's built 1:1 path. */
  void ForEachDirectPath(const std::function<void(CallDirectDriver&)>& fn);
  /** How a call's group path is built (empty: none — harness). */
  void BindHopPathFactory(CallHopPathFactory make) { resources_.make_hop = std::move(make); }
  /** Each call's built group path. */
  void ForEachHopPath(const std::function<void(CallHopDriver&)>& fn);

  /**
   * The call's media coordinator, created on first use. Null for a call this device never admitted
   * (or pruned), or before media resources are bound. An ended call keeps it for its stops.
   */
  CallMediaCoordinator* Media(const std::string& call_id);
  /** The call's coordinator if one exists (never creates it). */
  CallMediaCoordinator* Find(const std::string& call_id);
  /** Same, for readers (no sweep). */
  const CallMediaCoordinator* Peek(const std::string& call_id) const;
  /**
   * Stop `call_id`'s media (empty: whatever holds the seat). The seat's release tears the paths
   * down; with no seat, the hop and the call's 1:1 path stop their parts (the engine for a call
   * with neither). Also for calls not admitted here (leftovers).
   */
  void StopMedia(const std::string& call_id);
  /**
   * Another call is about to take media: stop media still running for any call but `keep_call_id`
   * (an ended call can leave the engine running). Never touches `keep_call_id`'s own media.
   */
  void StopMediaExcept(const std::string& keep_call_id);
  /** The engine runs (1:1 or group capture). */
  bool MediaRunning() const;
  /** The call the engine runs for; empty when it runs for none (or is stopped). */
  std::string MediaRunningCallId() const;

private:
  struct Entry {
    uint64_t instance = 0;
    std::unique_ptr<CallMediaCoordinator> coordinator;
  };

  /** Drop coordinators whose call LiveCalls no longer keeps. */
  void Sweep();

  const LiveCalls& live_;
  /** Outlives every coordinator (they hold a reference). */
  CallMediaResources resources_;
  std::map<std::string, Entry> media_;
};

} // namespace pbr
