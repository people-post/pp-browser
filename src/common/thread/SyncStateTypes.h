#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace pbr {

enum class PeerSyncPhase { Ok, Gap, Compromised };

struct SeqClosedRange {
  uint64_t min = 0;
  uint64_t max = 0;
};

/** Per-peer sync watermarks stored in thread.db sync_state.state_json (D037, D071). */
struct PeerSyncState {
  uint64_t contiguous_peer_seq = 0;
  uint64_t loaded_min_seq = 0;
  uint64_t loaded_max_seq = 0;
  uint64_t history_floor_seq = 0;
  PeerSyncPhase phase = PeerSyncPhase::Ok;
  std::optional<std::string> user_resolution;
  std::vector<uint64_t> empty_closed_seqs;
  std::vector<SeqClosedRange> empty_closed_ranges;
};

struct SeqRangeQuery {
  uint32_t session_epoch = 1;
  std::string seq_owner_contact_id;
  std::optional<uint64_t> min_sender_seq;
  std::optional<uint64_t> max_sender_seq;
  size_t limit = 50;
  bool ascending = true;
};

PeerSyncState DefaultPeerSyncState();

} // namespace pbr
