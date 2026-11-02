#pragma once

#include "feature/presenters/ThreadSearch.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace pbr {

/**
 * Search inside the open chat (DEV/PLAN-chat-search.md): what it shows and when its index must be (re)built.
 * It takes events and gives decisions; the caller builds the index off the UI thread and draws the view.
 * No threads, no GUI. The decrypted index lives here only while the search is open.
 */
class ThreadSearchSession {
public:
  /** Two index builds start at least this far apart (a busy chat would otherwise decrypt itself every message). */
  static constexpr int64_t kRebuildGapMs = 2000;

  struct View {
    bool open = false;
    /** The first index is being built: "Searching…". A rebuild keeps the old index usable and shows nothing. */
    bool indexing = false;
    std::string query;
    /** Index into hits(): 0 is the newest hit. */
    size_t current = 0;
    size_t hit_count = 0;
    bool can_older = false;
    bool can_newer = false;
    /** The message to scroll to: set when the query changed, on a step, or when the current hit moved. */
    std::optional<std::string> jump_to;
  };

  /** Opens with an empty query and asks for the first index: build it and pass this generation to IndexReady. */
  uint64_t Open();
  /** Drops the index, the hits and the query; a build still running for an older generation is ignored. */
  uint64_t Close();
  /**
   * A built index arrived. False (and dropped) when it is for another generation or the search is closed.
   * The hits are found again; the cursor stays on the same message when it is still a hit.
   */
  bool IndexReady(uint64_t generation, std::vector<ThreadSearchEntry> index, int64_t now_ms);
  /** The query changed: hits found again, cursor on the newest. */
  void SetQuery(std::string query);
  /** One hit older (up) / newer (down); stops at the ends. */
  void Older();
  void Newer();
  /** The message is gone from the store (deleted): drops its hit. False when it was not a hit. */
  bool DropHit(const std::string& message_id);
  /**
   * Messages of the chat were added or removed. Returns a generation to build when no build runs and the last one
   * finished at least kRebuildGapMs ago; otherwise remembers the rebuild for Tick.
   */
  std::optional<uint64_t> MessagesChanged(int64_t now_ms);
  /** Ask each frame (and after a build): a remembered rebuild that may start now gets its generation. */
  std::optional<uint64_t> Tick(int64_t now_ms);
  /** The view's jump_to, cleared: each jump is made once. */
  std::optional<std::string> TakeJump();

  const View& view() const { return view_; }
  const std::vector<ThreadSearchHit>& hits() const { return hits_; }
  /** The current hit's message ("" when there is none). */
  std::string CurrentMessageId() const;
  bool HasIndex() const { return !index_.empty(); }

private:
  /** Finds the hits again; `keep_cursor` follows the current message, else the cursor goes to the newest. */
  void Recompute(bool keep_cursor);
  void RefreshFlags();

  View view_;
  uint64_t generation_ = 0;
  bool building_ = false;
  bool rebuild_pending_ = false;
  std::optional<int64_t> last_built_ms_;
  std::vector<ThreadSearchEntry> index_;
  std::vector<ThreadSearchHit> hits_;
};

} // namespace pbr
