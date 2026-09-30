#pragma once

#include <cstddef>
#include <optional>
#include <string>

namespace pbr {

/**
 * The group (hop / media_relay) media path as a call's media coordinator drives it
 * (CallTopologyController implements it). The coordinator asks; the driver runs the hop.
 */
class CallHopDriver {
public:
  virtual ~CallHopDriver() = default;
  /** Our accept landed with `n_joined` in the call: true when the hop takes the call's media. */
  virtual bool OnLocalAcceptJoined(const std::string& call_id, size_t n_joined,
                                   const std::optional<std::string>& sfu_hint) = 0;
  /**
   * A peer accepted our call: true when the hop takes it (a group) — or the call is not the active
   * one; either way no 1:1 start.
   */
  virtual bool OnRemoteAcceptJoined(const std::string& call_id, size_t n_joined,
                                    const std::string& joiner_identity) = 0;
  /** Media runs on a hop now. */
  virtual bool IsSfuAttached() const = 0;
  /** A hop attach / migration / recovery is in flight. */
  virtual bool IsAwaitingSfuRecovery() const = 0;
  /** The call is (or is about to be) a group call on a hop: its 1:1 stream closing is expected. */
  virtual bool ExpectsGroupMedia(const std::string& call_id) const = 0;
  /** The 1:1 stream dropped ahead of the hop attach: wait for it (attach-wait). */
  virtual void BeginSfuAttachWait(const std::string& call_id) = 0;
  /** The call's media stopped: drop hop state for it — the no-seat stop path. */
  virtual void OnMediaStopped(const std::string& call_id) = 0;
  /** The camera was turned on / off: re-pick what the hop sends. */
  virtual void RefreshAdaptation(const std::string& call_id, bool camera_user_wants) = 0;
};

} // namespace pbr
