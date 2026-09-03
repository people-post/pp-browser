#include "feature/chat/WorkingSetController.h"

#include "domain/ui/ChatFormHelper.h"
#include "feature/ui/DataModelHost.h"

#include "common/ValueJson.h"

namespace pbr {

WorkingSetController::WorkingSetController(ShellView shell) : shell_(shell) {}

void WorkingSetController::BindShellNavigation(ShellNavigationPorts ports) {
  shell_navigation_ = std::move(ports);
}

void WorkingSetController::Dirty() {
  DataModelHost::Instance().Dirty("shell", "working_set_active");
  DataModelHost::Instance().Dirty("shell", "working_set_title");
  DataModelHost::Instance().Dirty("shell", "working_set_subtitle");
  DataModelHost::Instance().Dirty("shell", "working_set_rml");
  DataModelHost::Instance().Dirty("shell", "working_set");
}

std::vector<WorkingSetCandidate> WorkingSetController::HydrateCandidates(
    const std::vector<WorkingSetCandidate>& candidates, const std::string& entry_id) const {
  std::vector<WorkingSetCandidate> hydrated;
  hydrated.reserve(candidates.size());
  for (WorkingSetCandidate candidate : candidates) {
    candidate.artifact_rml = InjectEntryPlaceholders(candidate.artifact_rml, entry_id);
    candidate.teaser_rml = InjectEntryPlaceholders(candidate.teaser_rml, entry_id);
    hydrated.push_back(std::move(candidate));
  }
  return hydrated;
}

void WorkingSetController::SyncWidgetBindings(const std::string& entry_id) {
  shell_.working_set = {};
  if (widget_lookup_) {
    if (const TurnWidgetState* widgets = widget_lookup_(entry_id)) {
      shell_.working_set = *widgets;
    }
  }
  Dirty();
}

void WorkingSetController::Clear() {
  shell_.working_set_active = false;
  shell_.working_set_title = "";
  shell_.working_set_subtitle = "";
  shell_.working_set_rml = "";
  shell_.working_set = {};
  active_affinity_ = WorkingSetAffinity::None;
  active_entry_id_.clear();
  if (shell_navigation_.set_auxiliary_available) {
    shell_navigation_.set_auxiliary_available(false);
  }
  if (shell_navigation_.close_auxiliary) {
    shell_navigation_.close_auxiliary();
  }
  Dirty();
}

void WorkingSetController::ClearAll() {
  by_entry_.clear();
  Clear();
}

void WorkingSetController::Open(const std::string& entry_id, const int block_index) {
  const auto entry_it = by_entry_.find(entry_id);
  if (entry_it == by_entry_.end()) {
    return;
  }

  const WorkingSetCandidate* selected = nullptr;
  for (const WorkingSetCandidate& candidate : entry_it->second) {
    if (candidate.block_index == block_index) {
      selected = &candidate;
      break;
    }
  }
  if (!selected) {
    return;
  }

  shell_.working_set_active = true;
  shell_.working_set_title = Rml::String(selected->title.c_str());
  shell_.working_set_subtitle = Rml::String(selected->subtitle.c_str());
  shell_.working_set_rml = Rml::String(selected->artifact_rml.c_str());
  active_affinity_ = selected->affinity;
  active_entry_id_ = entry_id;
  SyncWidgetBindings(entry_id);

  if (shell_navigation_.set_auxiliary_available) {
    shell_navigation_.set_auxiliary_available(true);
  }
  if (shell_navigation_.open_auxiliary) {
    shell_navigation_.open_auxiliary();
  }
  Dirty();
}

void WorkingSetController::ApplyFromParse(const std::string& entry_id,
                                          const std::vector<WorkingSetCandidate>& candidates) {
  if (candidates.empty()) {
    Clear();
    return;
  }

  const std::vector<WorkingSetCandidate> hydrated = HydrateCandidates(candidates, entry_id);
  by_entry_[entry_id] = hydrated;

  const WorkingSetCandidate* primary = nullptr;
  for (const WorkingSetCandidate& candidate : hydrated) {
    if (candidate.auto_open) {
      primary = &candidate;
      break;
    }
  }
  if (!primary) {
    Clear();
    return;
  }

  const bool same_task = shell_.working_set_active && active_entry_id_ == entry_id &&
                         active_affinity_ == primary->affinity &&
                         active_affinity_ != WorkingSetAffinity::None;

  shell_.working_set_active = true;
  shell_.working_set_title = Rml::String(primary->title.c_str());
  shell_.working_set_subtitle = Rml::String(primary->subtitle.c_str());
  shell_.working_set_rml = Rml::String(primary->artifact_rml.c_str());
  active_affinity_ = primary->affinity;
  active_entry_id_ = entry_id;
  SyncWidgetBindings(entry_id);

  if (shell_navigation_.set_auxiliary_available) {
    shell_navigation_.set_auxiliary_available(true);
  }
  const bool auxiliary_open =
      shell_navigation_.snapshot ? shell_navigation_.snapshot().auxiliary_open : false;
  if (!same_task || !auxiliary_open) {
    if (shell_navigation_.open_auxiliary) {
      shell_navigation_.open_auxiliary();
    }
  }
  Dirty();
}

bool WorkingSetController::ShouldCloseForAction(const std::optional<std::string>& payload) const {
  if (!payload || payload->empty()) {
    return false;
  }
  auto doc = TryParseObject(*payload);
  if (!doc) {
    return false;
  }
  const std::string type = doc->getString("type").value_or("");
  return type == "start_conversation" || type == "add_contact";
}

} // namespace pbr
