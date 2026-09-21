#include "gui/chat/WorkingSetController.h"

#include "domain/ui/ChatFormHelper.h"
#include "gui/shell/DataModelHost.h"

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
  actions_by_entry_.clear();
  Clear();
}

void WorkingSetController::ShowUnavailable(const std::string& entry_id) {
  shell_.working_set_active = true;
  shell_.working_set_title = "Results unavailable";
  shell_.working_set_subtitle = "";
  shell_.working_set_rml =
      ui::String("<p class=\"muted\">These results are no longer available. Run the search again "
                 "from chat if you still need them.</p>");
  active_affinity_ = WorkingSetAffinity::None;
  active_entry_id_ = entry_id;
  shell_.working_set = {};
  if (shell_navigation_.set_auxiliary_available) {
    shell_navigation_.set_auxiliary_available(true);
  }
  if (shell_navigation_.open_auxiliary) {
    shell_navigation_.open_auxiliary();
  }
  Dirty();
}

void WorkingSetController::Open(const std::string& entry_id, const int block_index) {
  const auto entry_it = by_entry_.find(entry_id);
  if (entry_it == by_entry_.end()) {
    ShowUnavailable(entry_id);
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
    ShowUnavailable(entry_id);
    return;
  }

  shell_.working_set_active = true;
  shell_.working_set_title = ui::String(selected->title.c_str());
  shell_.working_set_subtitle = ui::String(selected->subtitle.c_str());
  shell_.working_set_rml = ui::String(selected->artifact_rml.c_str());
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
                                          const std::vector<WorkingSetCandidate>& candidates,
                                          std::vector<TranscriptChatAction> chat_actions) {
  if (candidates.empty()) {
    Clear();
    return;
  }

  const std::vector<WorkingSetCandidate> hydrated = HydrateCandidates(candidates, entry_id);
  by_entry_[entry_id] = hydrated;
  actions_by_entry_[entry_id] = std::move(chat_actions);

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
  shell_.working_set_title = ui::String(primary->title.c_str());
  shell_.working_set_subtitle = ui::String(primary->subtitle.c_str());
  shell_.working_set_rml = ui::String(primary->artifact_rml.c_str());
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

std::vector<WorkingSetCandidate> WorkingSetController::RestoreEntry(
    const std::string& entry_id, const std::vector<WorkingSetCandidate>& candidates,
    std::vector<TranscriptChatAction> chat_actions) {
  if (entry_id.empty() || candidates.empty()) {
    return {};
  }
  std::vector<WorkingSetCandidate> hydrated = HydrateCandidates(candidates, entry_id);
  by_entry_[entry_id] = hydrated;
  actions_by_entry_[entry_id] = std::move(chat_actions);
  if (shell_navigation_.set_auxiliary_available) {
    shell_navigation_.set_auxiliary_available(true);
  }
  return hydrated;
}

bool WorkingSetController::HasEntry(const std::string& entry_id) const {
  return by_entry_.find(entry_id) != by_entry_.end();
}

std::optional<TranscriptChatAction> WorkingSetController::LookupChatAction(const std::string& entry_id,
                                                                           const int action_index) const {
  if (action_index < 0 || entry_id.empty()) {
    return std::nullopt;
  }
  const auto it = actions_by_entry_.find(entry_id);
  if (it == actions_by_entry_.end()) {
    return std::nullopt;
  }
  if (static_cast<size_t>(action_index) >= it->second.size()) {
    return std::nullopt;
  }
  return it->second[static_cast<size_t>(action_index)];
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
