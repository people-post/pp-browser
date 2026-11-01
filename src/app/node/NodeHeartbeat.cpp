#include "app/node/NodeHeartbeat.h"

#include "common/Logger.h"
#include "common/ValueJson.h"
#include "common/metrics/MetricsRegistry.h"

#include <algorithm>
#include <cstdlib>
#include <utility>
#include "common/PbrCompat.h"

namespace pbr {
namespace {

constexpr int64_t kReregisterInitialMs = 10 * 1000;
constexpr int64_t kReregisterMaxMs = 5 * 60 * 1000;
constexpr int64_t kClockSkewWarnMs = 60 * 1000;

const char* OutcomeLabel(const int outcome) {
  static const char* const kLabels[] = {"ok",           "auth",       "stale",      "not_registered",
                                        "unknown_peer", "rejected",   "http_error", "network"};
  return kLabels[outcome];
}

struct ParsedError {
  std::string code;
  std::optional<int64_t> server_time;
};

ParsedError ParseBody(const std::string& body) {
  ParsedError out;
  if (const auto obj = TryParseObject(body)) {
    out.code = obj->getString("code").value_or(obj->getString("error").value_or(""));
    if (const auto t = obj->getNonNegInt("server_time")) {
      out.server_time = static_cast<int64_t>(*t);
    }
  }
  return out;
}

} // namespace

const char* HeartbeatStateName(const HeartbeatState state) {
  switch (state) {
  case HeartbeatState::Active:
    return "active";
  case HeartbeatState::Draining:
    return "draining";
  case HeartbeatState::Retiring:
    return "retiring";
  }
  return "active";
}

std::optional<HeartbeatState> ParseHeartbeatState(const std::string_view name) {
  for (const HeartbeatState s : {HeartbeatState::Active, HeartbeatState::Draining, HeartbeatState::Retiring}) {
    if (name == HeartbeatStateName(s)) {
      return s;
    }
  }
  return std::nullopt;
}

NodeHeartbeat::NodeHeartbeat(NodeHeartbeatDeps deps) : deps_(std::move(deps)) {
  // Series exist from start so absent() / rate() queries have a baseline.
  MetricsRegistry& r = MetricsRegistry::Global();
  for (int i = 0; i <= static_cast<int>(Outcome::Network); ++i) {
    r.Counter("pp_node_heartbeat_total", "Mesh heartbeats sent, by result.", {{"result", OutcomeLabel(i)}});
  }
  PublishState(HeartbeatState::Active);
}

void NodeHeartbeat::PublishState(const HeartbeatState state) {
  MetricsRegistry& r = MetricsRegistry::Global();
  for (const HeartbeatState s : {HeartbeatState::Active, HeartbeatState::Draining, HeartbeatState::Retiring}) {
    r.Gauge("pp_node_heartbeat_state", "Reported liveness state (1 = current).", {{"state", HeartbeatStateName(s)}})
        .Set(s == state ? 1 : 0);
  }
}

bool NodeHeartbeat::SetState(const HeartbeatState next) {
  int cur = state_.load();
  while (true) {
    if (cur == static_cast<int>(next)) {
      return true;
    }
    if (cur == static_cast<int>(HeartbeatState::Retiring)) {
      return false;
    }
    if (state_.compare_exchange_weak(cur, static_cast<int>(next))) {
      break;
    }
  }
  PublishState(next);
  logging::getLogger("pp-node").info << "heartbeat state -> " << HeartbeatStateName(next);
  return true;
}

void NodeHeartbeat::RecordOutcome(const Outcome outcome) {
  MetricsRegistry::Global()
      .Counter("pp_node_heartbeat_total", "Mesh heartbeats sent, by result.",
               {{"result", OutcomeLabel(static_cast<int>(outcome))}})
      .Inc();
}

NodeHeartbeat::Outcome NodeHeartbeat::Send(const HeartbeatState state, const long timeout_s) {
  auto log = logging::getLogger("pp-node");
  const HeartbeatIdentity id = deps_.identity();
  const std::string state_name = HeartbeatStateName(state);
  const int64_t now_ms = deps_.unix_ms();
  // The server accepts only strictly increasing timestamps; stay ahead even if the clock steps back.
  const int64_t timestamp = std::max(now_ms, last_timestamp_ + 1);
  last_timestamp_ = timestamp;

  auto signature = deps_.sign(id.relay_user_id, id.peer_id, state_name, timestamp);
  if (!signature) {
    log.error << "heartbeat sign failed: " << signature.error().message;
    RecordOutcome(Outcome::HttpError);
    return Outcome::HttpError;
  }
  Object body;
  body.set("relay_user_id", id.relay_user_id);
  body.set("peer_id", id.peer_id);
  body.set("state", state_name);
  body.set("timestamp", timestamp);
  body.set("signature", *signature);

  const auto response = deps_.post(deps_.base_url + "/v1/mesh/heartbeat", DumpJson(body),
                                   {{"Content-Type", "application/json"}}, timeout_s);
  if (!response) {
    log.warning << "heartbeat failed: " << response.error().message;
    RecordOutcome(Outcome::Network);
    return Outcome::Network;
  }

  const long status = response.value().status_code;
  const ParsedError parsed = ParseBody(response.value().body);
  MetricsRegistry& r = MetricsRegistry::Global();
  int64_t skew_ms = 0;
  if (parsed.server_time) {
    skew_ms = *parsed.server_time - now_ms;
    r.Gauge("pp_node_heartbeat_clock_skew_seconds", "Server time minus local time at the last heartbeat reply.")
        .Set(static_cast<double>(skew_ms) / 1000.0);
  }

  Outcome outcome = Outcome::HttpError;
  if (status >= 200 && status < 300) {
    outcome = Outcome::Ok;
    r.Gauge("pp_node_heartbeat_last_success_timestamp_seconds", "Unix seconds of the last accepted heartbeat.")
        .Set(static_cast<double>(now_ms) / 1000.0);
  } else if (status == 401) {
    outcome = Outcome::Auth;
    if (parsed.server_time && std::llabs(skew_ms) > kClockSkewWarnMs) {
      log.error << "heartbeat rejected (401): clock skew " << skew_ms / 1000 << "s vs server";
    } else {
      log.error << "heartbeat rejected (401): " << parsed.code;
    }
  } else if (status == 409) {
    outcome = Outcome::Stale;
    log.warning << "heartbeat timestamp not newer than the server's; waiting for next period";
  } else if (status == 404 && (parsed.code == "not_registered" || parsed.code == "registration_expired")) {
    outcome = Outcome::NotRegistered;
    log.warning << "heartbeat: " << parsed.code;
  } else if (status == 404 && parsed.code == "unknown_peer") {
    outcome = Outcome::UnknownPeer;
    log.warning << "heartbeat: peer_id is not an endpoint of this registration; will retry";
  } else if (status == 400 || status == 403) {
    outcome = Outcome::Rejected;
    log.error << "heartbeat refused (" << status << "): " << parsed.code;
  } else {
    log.warning << "heartbeat http " << status;
  }
  RecordOutcome(outcome);
  return outcome;
}

void NodeHeartbeat::HandleNotRegistered(const int64_t now) {
  if (now < reregister_due_ms_) {
    next_send_ms_ = reregister_due_ms_;
    return;
  }
  // Backoff keeps a failing register/start from being hammered; a successful heartbeat resets it.
  reregister_delay_ms_ = reregister_delay_ms_ == 0 ? kReregisterInitialMs
                                                   : std::min(reregister_delay_ms_ * 2, kReregisterMaxMs);
  reregister_due_ms_ = now + reregister_delay_ms_;
  auto renewed = deps_.reregister();
  if (renewed) {
    next_send_ms_ = now;  // retry the heartbeat right away
  } else {
    logging::getLogger("pp-node").warning << "heartbeat re-registration failed: " << renewed.error().message;
    next_send_ms_ = reregister_due_ms_;
  }
}

void NodeHeartbeat::Tick() {
  const HeartbeatState state = State();
  const int64_t now = deps_.steady_ms();
  if (state == attempted_state_ && now < next_send_ms_) {
    return;
  }
  const HeartbeatIdentity id = deps_.identity();
  if (id.relay_user_id.empty() || id.peer_id.empty()) {
    return;
  }
  attempted_state_ = state;
  next_send_ms_ = now + kHeartbeatPeriodMs;
  const Outcome outcome = Send(state, kHeartbeatTimeoutS);
  if (outcome == Outcome::Ok) {
    reregister_delay_ms_ = 0;
    reregister_due_ms_ = INT64_MIN;
  } else if (outcome == Outcome::NotRegistered) {
    HandleNotRegistered(now);
  }
}

void NodeHeartbeat::SendFinal() {
  SetState(HeartbeatState::Retiring);
  const HeartbeatIdentity id = deps_.identity();
  if (id.relay_user_id.empty() || id.peer_id.empty()) {
    return;
  }
  Send(HeartbeatState::Retiring, kHeartbeatFinalTimeoutS);
}

} // namespace pbr
