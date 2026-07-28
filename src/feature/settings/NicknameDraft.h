#pragma once

#include <string>

namespace pbr {

/**
 * Intent-gated nickname draft for Me settings.
 *
 * committed — last value loaded from / saved to IdentityStore
 * draft     — value shown in the input (may diverge while editing)
 * ready     — identity nickname has been loaded at least once
 * edited    — user changed the field since last hydrate/commit (not remount noise)
 *
 * Rules:
 * - Hydrate from identity only updates draft when !edited
 * - Commit only when ready && edited && draft != committed
 * - Identity-unavailable while already ready leaves committed/draft alone
 */
struct NicknameDraft {
  std::string committed;
  std::string draft;
  bool ready = false;
  bool edited = false;

  /** Identity nickname available (messaging unlocked). */
  void OnHydrated(const std::string& from_identity);

  /** Identity not readable yet; no-op once ready so empty UI cannot wipe state. */
  void OnIdentityUnavailable();

  /** Real user edit (change event while not hydrating). No-op until ready. */
  void OnUserEdit(const std::string& value);

  /** True when a flush should write draft to IdentityStore. */
  bool ShouldCommit() const;

  /** After a successful flush of draft (or prefs-only flush that re-synced identity). */
  void OnCommitSuccess(const std::string& saved);

  /** Discard uncommitted edits and show committed (pane (re)open). */
  void ResetEditsToCommitted();
};

} // namespace pbr
