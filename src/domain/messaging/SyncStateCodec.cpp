#include "domain/messaging/SyncStateCodec.h"

#include "common/ValueJson.h"

#include <cstdint>
#include "common/PbrCompat.h"

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
  Object json;
  json.setJsonUInt("contiguous_peer_seq", state.contiguous_peer_seq);
  json.setJsonUInt("loaded_min_seq", state.loaded_min_seq);
  json.setJsonUInt("loaded_max_seq", state.loaded_max_seq);
  json.setJsonUInt("history_floor_seq", state.history_floor_seq);
  json.set("sync_state", PeerSyncPhaseToString(state.phase));
  if (state.user_resolution) {
    json.set("user_resolution", *state.user_resolution);
  } else {
    json.set("user_resolution", Null{});
  }
  std::vector<Value> empty_closed_seqs;
  empty_closed_seqs.reserve(state.empty_closed_seqs.size());
  for (const uint64_t seq : state.empty_closed_seqs) {
    if (seq <= static_cast<uint64_t>(INT64_MAX)) {
      empty_closed_seqs.push_back(Value(static_cast<int64_t>(seq)));
    } else {
      empty_closed_seqs.push_back(Value(seq));
    }
  }
  json.set("empty_closed_seqs", ArrayValue(std::move(empty_closed_seqs)));
  std::vector<Value> ranges;
  ranges.reserve(state.empty_closed_ranges.size());
  for (const SeqClosedRange& range : state.empty_closed_ranges) {
    Object row;
    row.setJsonUInt("min", range.min);
    row.setJsonUInt("max", range.max);
    ranges.push_back(ObjectValue(std::move(row)));
  }
  json.set("empty_closed_ranges", ArrayValue(std::move(ranges)));
  return DumpJson(json);
}

Roe<PeerSyncState> PeerSyncStateFromJson(const std::string& json_text) {
  auto json = TryParseObject(json_text);
  if (!json) {
    return Error("Invalid sync_state JSON");
  }

  PeerSyncState state;
  if (auto v = json->getNonNegInt("contiguous_peer_seq")) {
    state.contiguous_peer_seq = *v;
  }
  if (auto v = json->getNonNegInt("loaded_min_seq")) {
    state.loaded_min_seq = *v;
  }
  if (auto v = json->getNonNegInt("loaded_max_seq")) {
    state.loaded_max_seq = *v;
  }
  if (auto v = json->getNonNegInt("history_floor_seq")) {
    state.history_floor_seq = *v;
  }
  if (auto phase = json->getString("sync_state")) {
    state.phase = PeerSyncPhaseFromString(*phase);
  }
  if (auto resolution = json->getString("user_resolution")) {
    state.user_resolution = *resolution;
  }
  if (const Array* seqs = json->getArray("empty_closed_seqs")) {
    for (const Value& item : seqs->elements) {
      if (auto seq = asNonNegInt(item)) {
        state.empty_closed_seqs.push_back(*seq);
      }
    }
  }
  if (const Array* ranges = json->getArray("empty_closed_ranges")) {
    for (const Value& item : ranges->elements) {
      const Object* row = asObject(item);
      if (!row) {
        continue;
      }
      SeqClosedRange range;
      if (auto min = row->getNonNegInt("min")) {
        range.min = *min;
      }
      if (auto max = row->getNonNegInt("max")) {
        range.max = *max;
      }
      state.empty_closed_ranges.push_back(range);
    }
  }
  return state;
}

} // namespace pbr
