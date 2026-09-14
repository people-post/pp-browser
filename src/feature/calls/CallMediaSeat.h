#pragma once

#include "common/Module.h"

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * V036 — exclusive process-wide bind between call_id and call media.
 * SoftMigrate = NotePath(Hop) under the same token (no Release).
 * Leave / leftover purge = Release (Detach then Stop).
 */
class CallMediaSeat : public Module {
public:
  enum class PathKind { None = 0, Direct, Hop };

  struct Token {
    uint64_t epoch = 0;
    std::string call_id;
  };

  /** Topology OnMediaStopped (Detach + clear attach state). */
  using TopologyStoppedFn = std::function<void(const std::string& call_id)>;
  /**
   * Engine Stop on UI (bridge StopMeshMedia body).
   * When force=false: no-op if Epoch() != epoch_at_post (NoteStart / Acquire advanced).
   * When force=true: always stop (Acquire releasing a prior bind).
   */
  using StopEngineFn =
      std::function<void(const std::string& call_id, uint64_t epoch_at_post, bool force)>;

  CallMediaSeat();

  void SetTeardownHooks(TopologyStoppedFn topology_stopped, StopEngineFn stop_engine);

  /** Exclusive bind. Releases any other bound call first. */
  Token Acquire(const std::string& call_id);
  /** Full teardown for call_id (or current bind if call_id empty and bound). */
  void Release(const std::string& call_id);
  /** No-op when token.epoch != current Epoch(). */
  void Release(const Token& token);

  /**
   * Duplex StartSfu succeeded (or send-swap) for call_id — bump epoch so in-flight
   * Release cannot tear down the new session.
   */
  void NoteStart(const std::string& call_id);
  void NotePath(PathKind kind);

  bool IsBound(const std::string& call_id) const;
  std::string BoundCallId() const;
  uint64_t Epoch() const;
  PathKind Path() const;
  Token CurrentToken() const;

private:
  void InvokeTeardown(const std::string& call_id, uint64_t epoch_at_post, bool force);

  mutable std::mutex mu_;
  TopologyStoppedFn topology_stopped_;
  StopEngineFn stop_engine_;
  std::string bound_call_id_;
  uint64_t epoch_ = 0;
  PathKind path_ = PathKind::None;
};

} // namespace pbr
