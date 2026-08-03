#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

namespace pbr {

/** Well-known field ids for UiEditSession baselines. */
inline constexpr const char kUiFieldProfileNickname[] = "settings.profile_nickname";

/**
 * Same-thread guard for data-value text fields (blur / leave-surface commit).
 *
 * The UI loop is single-threaded, but remount + Dirty + Context::Update re-enter:
 *   change → Dirty → SetValue → blur → Flush → DirtyAll/DirtyNavChrome → remount → …
 *
 * Policy:
 * - BeginRemount / EndRemount: nestable; commits ignored while remount_depth_ > 0
 * - OnLoaded / ShouldCommit / OnCommitted: persist only when live ≠ baseline
 * - ShouldPushToView: do not Dirty/SetValue a field that is mid-edit
 */
class UiEditSession {
public:
  UiEditSession() = default;

  static UiEditSession& Instance();

  void BeginRemount();
  void EndRemount();
  bool RemountBlocking() const { return remount_depth_ > 0; }
  uint64_t RemountEpoch() const { return remount_epoch_; }

  void OnLoaded(const std::string& field_id, const std::string& value);
  bool HasBaseline(const std::string& field_id) const;
  /** nullptr if never loaded. */
  const std::string* Baseline(const std::string& field_id) const;

  bool IsMidEdit(const std::string& field_id, const std::string& live) const;
  /** True when commits are allowed and live differs from baseline. */
  bool ShouldCommit(const std::string& field_id, const std::string& live) const;
  /** False while mid-edit — caller should skip Dirty(field). */
  bool ShouldPushToView(const std::string& field_id, const std::string& live) const;

  void OnCommitted(const std::string& field_id, const std::string& value);

  /**
   * Value to put in bindings after a load/sync.
   * Preserves live when mid-edit; otherwise uses loaded.
   */
  std::string ResolveAfterLoad(const std::string& field_id, const std::string& loaded,
                               const std::string& live_binding) const;

private:
  uint64_t remount_epoch_ = 0;
  int remount_depth_ = 0;
  std::unordered_map<std::string, std::string> baselines_;
};

} // namespace pbr
