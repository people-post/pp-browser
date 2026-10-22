#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace pbr {

/**
 * The 1:1 call-media control channel's wire messages (JSON, v1), both directions: the hello that
 * opens a call's bundle, k3 path migration (migrate / path_add and their ack, path release) and
 * the k4 heartbeat. CallMediaLegCoordinator sends and routes them.
 */
enum class CallMediaControlType {
  Unknown,
  Hello,
  HelloAck,
  Heartbeat,
  /** Move the call onto the new path. */
  Migrate,
  /** k6: keep the new path as the call's standby. */
  PathAdd,
  MigrateAck,
  PathRelease,
  PathReleaseAck,
};

struct CallMediaControl {
  CallMediaControlType type = CallMediaControlType::Unknown;
  std::string call_id;
  /** Hello: 1 when absent; migrate / path_add: 0 when absent. */
  uint32_t media_epoch = 0;
  uint32_t path_gen = 0;
  /** Hello: the sender is the call's offerer. */
  bool sender_offerer = false;
  /** Acks. */
  bool ok = false;
  std::string error;
  /** Heartbeat: sent on the sender's active path. */
  bool active = false;
};

/** None when `json` is not a JSON object; an unknown `type` decodes as Unknown. */
std::optional<CallMediaControl> DecodeCallMediaControl(const std::string& json);

std::string EncodeCallMediaHello(const std::string& call_id, uint32_t media_epoch, bool offerer);
std::string EncodeCallMediaHelloAck(bool ok, const char* error = nullptr);
const std::string& EncodeCallMediaHeartbeat(bool active);
/** `standby_only`: path_add (k6) instead of migrate. */
std::string EncodeCallMediaMigrate(const std::string& call_id, uint32_t media_epoch, uint32_t path_gen,
                                   bool standby_only);
std::string EncodeCallMediaMigrateAck(bool ok, uint32_t path_gen, const std::string& error = {});
/** `ack`: path_release_ack instead of path_release. */
std::string EncodeCallMediaPathRelease(bool ack, uint32_t path_gen);

} // namespace pbr
