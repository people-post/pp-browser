#pragma once

#include "base/ui/ChatWidgetTypes.h"
#include "common/WorkingSetTypes.h"
#include "feature/ui/ShellNavigationPorts.h"

#include <RmlUi/Core/Types.h>

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
    Rml::String& working_set_title;
    Rml::String& working_set_subtitle;
    Rml::String& working_set_rml;
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
  void ApplyFromParse(const std::string& entry_id, const std::vector<WorkingSetCandidate>& candidates);
  bool ShouldCloseForAction(const std::optional<std::string>& payload) const;
  void SyncWidgetBindings(const std::string& entry_id);
  const std::string& ActiveEntryId() const { return active_entry_id_; }

private:
  void Dirty();
  std::vector<WorkingSetCandidate> HydrateCandidates(const std::vector<WorkingSetCandidate>& candidates,
                                                     const std::string& entry_id) const;

  ShellView shell_;
  WidgetLookup widget_lookup_;
  ShellNavigationPorts shell_navigation_;
  std::map<std::string, std::vector<WorkingSetCandidate>> by_entry_;
  WorkingSetAffinity active_affinity_ = WorkingSetAffinity::None;
  std::string active_entry_id_;
};

} // namespace pbr
