#include "base/messaging/SyncStateCodec.h"

#include <nlohmann/json.hpp>

namespace pbr {

PeerSyncState DefaultPeerSyncState() { return PeerSyncState{}; }

std::string PeerSyncPhaseToString(const PeerSyncPhase phase) {
  switch (phase) {
  case PeerSyncPhase::Gap:
    return "gap";
  case PeerSyncPhase::Compromised:
    return "compromised";
  case PeerSyncPhase::Ok:
    return "ok";
  }
  return "ok";
}

PeerSyncPhase PeerSyncPhaseFromString(const std::string& value) {
  if (value == "gap") {
    return PeerSyncPhase::Gap;
  }
  if (value == "compromised") {
    return PeerSyncPhase::Compromised;
  }
  return PeerSyncPhase::Ok;
}

std::string PeerSyncStateToJson(const PeerSyncState& state) {
  nlohmann::json json = {{"contiguous_peer_seq", state.contiguous_peer_seq},
                         {"loaded_min_seq", state.loaded_min_seq},
                         {"loaded_max_seq", state.loaded_max_seq},
                         {"history_floor_seq", state.history_floor_seq},
                         {"sync_state", PeerSyncPhaseToString(state.phase)},
                         {"empty_closed_seqs", state.empty_closed_seqs},
                         {"empty_closed_ranges", nlohmann::json::array()}};
  if (state.user_resolution) {
    json["user_resolution"] = *state.user_resolution;
  } else {
    json["user_resolution"] = nullptr;
  }
  for (const SeqClosedRange& range : state.empty_closed_ranges) {
    json["empty_closed_ranges"].push_back({{"min", range.min}, {"max", range.max}});
  }
  return json.dump();
}

Roe<PeerSyncState> PeerSyncStateFromJson(const std::string& json_text) {
  nlohmann::json json;
  try {
    json = nlohmann::json::parse(json_text);
  } catch (const std::exception&) {
    return Error("Invalid sync_state JSON");
  }

  PeerSyncState state;
  if (json.contains("contiguous_peer_seq") && json["contiguous_peer_seq"].is_number_unsigned()) {
    state.contiguous_peer_seq = json["contiguous_peer_seq"].get<uint64_t>();
  }
  if (json.contains("loaded_min_seq") && json["loaded_min_seq"].is_number_unsigned()) {
    state.loaded_min_seq = json["loaded_min_seq"].get<uint64_t>();
  }
  if (json.contains("loaded_max_seq") && json["loaded_max_seq"].is_number_unsigned()) {
    state.loaded_max_seq = json["loaded_max_seq"].get<uint64_t>();
  }
  if (json.contains("history_floor_seq") && json["history_floor_seq"].is_number_unsigned()) {
    state.history_floor_seq = json["history_floor_seq"].get<uint64_t>();
  }
  if (json.contains("sync_state") && json["sync_state"].is_string()) {
    state.phase = PeerSyncPhaseFromString(json["sync_state"].get<std::string>());
  }
  if (json.contains("user_resolution") && json["user_resolution"].is_string()) {
    state.user_resolution = json["user_resolution"].get<std::string>();
  }
  if (json.contains("empty_closed_seqs") && json["empty_closed_seqs"].is_array()) {
    for (const auto& item : json["empty_closed_seqs"]) {
      if (item.is_number_unsigned()) {
        state.empty_closed_seqs.push_back(item.get<uint64_t>());
      }
    }
  }
  if (json.contains("empty_closed_ranges") && json["empty_closed_ranges"].is_array()) {
    for (const auto& item : json["empty_closed_ranges"]) {
      if (!item.is_object()) {
        continue;
      }
      SeqClosedRange range;
      if (item.contains("min") && item["min"].is_number_unsigned()) {
        range.min = item["min"].get<uint64_t>();
      }
      if (item.contains("max") && item["max"].is_number_unsigned()) {
        range.max = item["max"].get<uint64_t>();
      }
      state.empty_closed_ranges.push_back(range);
    }
  }
  return state;
}

} // namespace pbr
