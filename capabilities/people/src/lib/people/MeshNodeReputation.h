#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

namespace pbr {

/**
 * What this profile saw mesh nodes do wrong (N032 rules 6 and 8): a node whose claim was contradicted, and its
 * operator account, are demoted for a while. Local only — never shared, never read from another client — and
 * persisted per profile. It only ever demotes: a node is never ranked higher for anything recorded here.
 *
 * A demotion lasts kFirstDemotionMs, doubling with each strike up to kMaxDemotionMs; a node or operator with no strike
 * for kForgetAfterMs starts over. A contradiction seen again while still demoted is not a new strike (the audit runs
 * every policy refresh). Wall-clock times, so they survive a restart. Thread-safe.
 */
class MeshNodeReputation {
public:
  static constexpr int64_t kDayMs = 24LL * 60 * 60 * 1000;
  static constexpr int64_t kFirstDemotionMs = 7 * kDayMs;
  static constexpr int64_t kMaxDemotionMs = 30 * kDayMs;
  static constexpr int64_t kForgetAfterMs = 90 * kDayMs;
  /** Per kind (nodes, operators): past it the entry touched longest ago is forgotten. */
  static constexpr size_t kMaxEntries = 512;

  static int64_t NowMs();

  /** Where it persists (`{profile data dir}/mesh_node_reputation.json`); empty keeps it in memory. */
  void SetPersistPath(std::string path);
  /** Read the persisted file, if any (a file of another schema is ignored). */
  void Load();

  /** A claim of node `peer_id` was contradicted; its operator `account_id` (may be empty) is demoted with it. */
  void NoteContradiction(const std::string& peer_id, const std::string& account_id, int64_t now_ms);
  /** The node, or its operator, is demoted now. */
  bool IsDemoted(const std::string& peer_id, const std::string& account_id, int64_t now_ms) const;
  int Strikes(const std::string& peer_id) const;

private:
  struct Entry {
    int64_t demoted_until_ms = 0;
    int64_t last_strike_ms = 0;
    int strikes = 0;
  };
  using Book = std::unordered_map<std::string, Entry>;

  /** True when this added a strike (it was not already demoted). */
  static bool Strike(Book& book, const std::string& key, int64_t now_ms);
  static bool DemotedIn(const Book& book, const std::string& key, int64_t now_ms);
  void PersistLocked() const;

  mutable std::mutex mu_;
  Book nodes_;
  Book operators_;
  std::string path_;
};

} // namespace pbr
