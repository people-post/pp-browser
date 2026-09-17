#pragma once

#include "domain/ui/ChatWidgetTypes.h"
#include "common/chat/ChatActionTypes.h"
#include "common/ui/WorkingSetTypes.h"
#include "gui/shell/ShellNavigationPorts.h"

#include <ui/base/Types.h>

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace pbr {

/** Auxiliary working-set panel sticky rules and open/clear lifecycle. */
class WorkingSetController {
public:
  struct ShellView {
    bool& working_set_active;
    ui::String& working_set_title;
    ui::String& working_set_subtitle;
    ui::String& working_set_rml;
    TurnWidgetState& working_set;
  };

  using WidgetLookup = std::function<const TurnWidgetState*(const std::string& entry_id)>;

  explicit WorkingSetController(ShellView shell);

  void BindShellNavigation(ShellNavigationPorts ports);
  void SetWidgetLookup(WidgetLookup lookup) { widget_lookup_ = std::move(lookup); }

  void Clear();
  /** Clear panel state and forget all entry candidates (thread switch / shutdown). */
  void ClearAll();
  void Open(const std::string& entry_id, int block_index);
  void ApplyFromParse(const std::string& entry_id, const std::vector<WorkingSetCandidate>& candidates,
                      std::vector<TranscriptChatAction> chat_actions = {});
  /**
   * Rehydrate panel maps from a persisted assistant message without auto-opening
   * (used after thread switch). Returns hydrated candidates for optional persistence rewrite.
   */
  std::vector<WorkingSetCandidate> RestoreEntry(const std::string& entry_id,
                                                const std::vector<WorkingSetCandidate>& candidates,
                                                std::vector<TranscriptChatAction> chat_actions = {});
  bool HasEntry(const std::string& entry_id) const;
  /** Resolve an action from the in-memory panel table (preferred over thread-store lookup). */
  std::optional<TranscriptChatAction> LookupChatAction(const std::string& entry_id, int action_index) const;
  bool ShouldCloseForAction(const std::optional<std::string>& payload) const;
  void SyncWidgetBindings(const std::string& entry_id);
  const std::string& ActiveEntryId() const { return active_entry_id_; }

  /** Hydrate `__ENTRY__` placeholders; public so FinishAssistantReply can persist the same RML. */
  std::vector<WorkingSetCandidate> HydrateCandidates(const std::vector<WorkingSetCandidate>& candidates,
                                                     const std::string& entry_id) const;

private:
  void Dirty();
  void ShowUnavailable(const std::string& entry_id);

  ShellView shell_;
  WidgetLookup widget_lookup_;
  ShellNavigationPorts shell_navigation_;
  std::map<std::string, std::vector<WorkingSetCandidate>> by_entry_;
  std::map<std::string, std::vector<TranscriptChatAction>> actions_by_entry_;
  WorkingSetAffinity active_affinity_ = WorkingSetAffinity::None;
  std::string active_entry_id_;
};

} // namespace pbr
