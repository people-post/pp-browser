#pragma once

#include "lib/calls/LiveCall.h"
#include "feature/calls/CallMediaCoordinator.h"

#include <cstdint>
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
  void BindMediaResources(CallMediaEngine* engine, CallMediaSeat* seat);
  /** The 1:1 path the coordinators drive (null: none wired). */
  void BindDirectDriver(CallDirectDriver* direct) { resources_.direct = direct; }
  /** The group path the coordinators drive (null: harness). */
  void BindHopDriver(CallHopDriver* hop) { resources_.hop = hop; }

  /**
   * The call's media coordinator, created on first use. Null for a call this device never admitted
   * (or pruned), or before media resources are bound. An ended call keeps it for its stops.
   */
  CallMediaCoordinator* Media(const std::string& call_id);
  /**
   * Stop `call_id`'s media (empty: whatever holds the seat). The seat's release tears the paths
   * down; with no seat, each driver stops its part. Also for calls not admitted here (leftovers).
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
