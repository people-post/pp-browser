#include <stdexcept>
#include "feature/ui/PeoplePickerController.h"

#include "base/i18n/LocalizationService.h"
#include "base/messaging/CallSessionLogic.h"
#include "base/messaging/CallTypes.h"
#include "base/messaging/DirectChatTarget.h"
#include "base/messaging/GroupTypes.h"
#include "base/messaging/ThreadTypes.h"
#include "base/people/ContactJson.h"
#include "base/people/ContactTypes.h"
#include "base/people/PeerDisplayLabel.h"
#include "base/ui/ShellTypes.h"
#include "feature/messaging/ContactReachability.h"
#include "feature/ui/CallController.h"
#include "feature/ui/ChatSessionPorts.h"
#include "feature/ui/DataModelHost.h"
#include "feature/ui/FlowCoordinator.h"
#include "base/crypto/ProfileUnlockGate.h"
#include "feature/ui/UserFeedback.h"

#include <algorithm>
#include <cctype>
#include <optional>

namespace pbr {
namespace {

constexpr const char* kStepSelect = "select";
constexpr const char* kStepName = "name";

bool ContactIsSelectable(const Contact& contact, const MessagingContactsPorts* ports) {
  if (contact.trust == TrustLevel::Blocked) {
    return false;
  }
  const DirectChatTarget target = DirectChatTargetFromContact(contact, ThreadChannel::E2ePublic);
  if (target.peer_identity_value.empty()) {
    return false;
  }
  if (ports != nullptr && ports->snapshot && ports->snapshot().messaging_ready && ports->is_contact_reachable) {
    return ports->is_contact_reachable(contact);
  }
  if (target.peer_identity_kind == ContactIdKindToString(ContactIdKind::PeerId) && contact.multiaddrs.empty()) {
    return false;
  }
  return true;
}

std::string ContactSubtitle(const Contact& contact) {
  for (const ContactId& id : contact.ids) {
    if (id.kind == ContactIdKind::RelayUser && id.primary) {
      return ShortRelayId(id.value);
    }
  }
  for (const ContactId& id : contact.ids) {
    if (id.kind == ContactIdKind::RelayUser) {
      return ShortRelayId(id.value);
    }
  }
  for (const ContactId& id : contact.ids) {
    if (id.kind == ContactIdKind::PeerId) {
      return ShortRelayId(id.value);
    }
  }
  if (!contact.server_nickname.empty() && contact.server_nickname != contact.display_name) {
    return contact.server_nickname;
  }
  return {};
}

bool ContainsIgnoreCase(const std::string& hay, const std::string& query_lower) {
  if (query_lower.empty()) {
    return true;
  }
  std::string lower = hay;
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return lower.find(query_lower) != std::string::npos;
}

bool MatchesQuery(const Contact& contact, const std::string& query_lower) {
  if (query_lower.empty()) {
    return true;
  }
  if (ContainsIgnoreCase(contact.display_name, query_lower) ||
      ContainsIgnoreCase(contact.server_nickname, query_lower)) {
    return true;
  }
  for (const ContactId& id : contact.ids) {
    if (ContainsIgnoreCase(id.value, query_lower)) {
      return true;
    }
  }
  return false;
}

bool MatchesIdentityQuery(const std::string& title, const std::string& identity,
                          const std::string& query_lower) {
  if (query_lower.empty()) {
    return true;
  }
  return ContainsIgnoreCase(title, query_lower) || ContainsIgnoreCase(identity, query_lower);
}

} // namespace

PeoplePickerController::PeoplePickerController() {
  redirectLogger("PeoplePickerController");
}

PeoplePickerController& PeoplePickerController::Instance() {
  static PeoplePickerController controller;
  return controller;
}
void PeoplePickerController::BindContactsPorts(MessagingContactsPorts ports) {
  contacts_ports_ = std::move(ports);
}

void PeoplePickerController::BindPickerPorts(MessagingPeoplePickerPorts ports) {
  picker_ports_ = std::move(ports);
}

void PeoplePickerController::BindUnlockGate(ProfileUnlockGate& unlock_gate) {
  unlock_gate_ = &unlock_gate;
}

void PeoplePickerController::BindChatPorts(ChatSessionPorts ports) {
  chat_ports_ = std::move(ports);
}

void PeoplePickerController::BindFlowCoordinator(FlowCoordinator& flow) {
  flow_ = &flow;
}

void PeoplePickerController::BindCallController(CallController& call) {
  call_ = &call;
}

void PeoplePickerController::BindShellNavigation(ShellNavigationPorts ports) {
  shell_navigation_ = std::move(ports);
}

void PeoplePickerController::BindShellFeedback(ShellFeedbackPorts ports) {
  shell_feedback_ = std::move(ports);
}

void PeoplePickerController::ShellDirty() {
  if (shell_navigation_.dirty_window) {
    shell_navigation_.dirty_window();
  }
}

void PeoplePickerController::ShowToast(const std::string& message, const ToastDuration duration) {
  if (shell_feedback_.show_toast) {
    shell_feedback_.show_toast(message, duration);
  }
}

bool PeoplePickerController::MessagingInitialized() const {
  return contacts_ports_.snapshot && contacts_ports_.snapshot().initialized;
}

bool PeoplePickerController::MessagingReady() const {
  return contacts_ports_.snapshot && contacts_ports_.snapshot().messaging_ready;
}

bool PeoplePickerController::RegisterModel(Rml::Context* context) {
  if (!context) {
    return false;
  }
  context_ = context;

  return DataModelHost::Instance().Register(context, "people_picker", [](Rml::DataModelConstructor& ctor) {
    auto& controller = PeoplePickerController::Instance();
    if (auto row_handle = ctor.RegisterStruct<PickerRow>()) {
      row_handle.RegisterMember("id", &PickerRow::id);
      row_handle.RegisterMember("title", &PickerRow::title);
      row_handle.RegisterMember("subtitle", &PickerRow::subtitle);
      row_handle.RegisterMember("selected", &PickerRow::selected);
      row_handle.RegisterMember("locked", &PickerRow::locked);
    }
    if (auto member_handle = ctor.RegisterStruct<MemberSummaryRow>()) {
      member_handle.RegisterMember("title", &MemberSummaryRow::title);
    }
    ctor.RegisterArray<std::vector<PickerRow>>();
    ctor.RegisterArray<std::vector<MemberSummaryRow>>();
    ctor.Bind("rows", &controller.rows_);
    ctor.Bind("member_summary", &controller.member_summary_);
    ctor.Bind("search_query", &controller.search_query_);
    ctor.Bind("title", &controller.title_);
    ctor.Bind("step", &controller.step_);
    ctor.Bind("group_title", &controller.group_title_);
    ctor.Bind("group_title_help", &controller.group_title_help_);
    ctor.Bind("cta_label", &controller.cta_label_);
    ctor.Bind("cta_enabled", &controller.cta_enabled_);
    ctor.Bind("empty_hint", &controller.empty_hint_);
    ctor.BindEventCallback("toggle_contact", &PeoplePickerController::ToggleContactCallback);
    ctor.BindEventCallback("on_search_changed", &PeoplePickerController::OnSearchChangedCallback);
    ctor.BindEventCallback("confirm_picker", &PeoplePickerController::ConfirmCallback);
    ctor.BindEventCallback("cancel_picker", &PeoplePickerController::CancelCallback);
    ctor.BindEventCallback("back_picker", &PeoplePickerController::BackCallback);
    ctor.BindEventCallback("create_group", &PeoplePickerController::CreateGroupCallback);
  });
}

void PeoplePickerController::OpenFree() {
  Open(PeoplePickerMode::Free, {});
}

void PeoplePickerController::OpenFromDm(const std::string& locked_contact_id) {
  if (locked_contact_id.empty()) {
    UserFeedback::Fail("No contact for this chat");
    ShellDirty();
    return;
  }
  Open(PeoplePickerMode::FromDm, {locked_contact_id});
}

void PeoplePickerController::OpenForGroupCall(const std::string& thread_id, const bool video) {
  if (!MessagingInitialized() || !picker_ports_.get_thread) {
    UserFeedback::Fail("Messaging not ready");
    return;
  }
  auto thread = picker_ports_.get_thread(thread_id);
  if (!thread || !*thread || (*thread)->kind != ThreadKind::Group || !(*thread)->group_id) {
    UserFeedback::Fail("Group chat required");
    return;
  }
  Close();
  mode_ = PeoplePickerMode::GroupCall;
  call_thread_id_ = thread_id;
  call_id_.clear();
  call_video_ = video;
  locked_ids_.clear();
  selected_ids_.clear();
  identity_for_contact_.clear();
  search_query_ = "";
  step_ = kStepSelect;
  title_ = call_video_ ? Tr("people_picker.title_group_video_call").c_str()
                       : Tr("people_picker.title_group_voice_call").c_str();
  empty_hint_ = Tr("people_picker.empty_group_call").c_str();
  SyncGroupCallRows();
  for (PickerRow& row : rows_) {
    selected_ids_.insert(row.id.c_str());
    row.selected = true;
  }
  UpdateCta();
  DirtyAll();
  PaneSpec spec;
  spec.key = "people_picker";
  layer_id_ = shell_navigation_.push_layer ? shell_navigation_.push_layer(spec) : -1;
  RegisterFlow();
  ShellDirty();
}

void PeoplePickerController::OpenForCallAddGuest(const std::string& call_id) {
  if (!MessagingInitialized() || call_id.empty()) {
    UserFeedback::Fail("Calls unavailable");
    return;
  }
  Close();
  mode_ = PeoplePickerMode::CallAddGuest;
  call_thread_id_.clear();
  call_id_ = call_id;
  call_video_ = false;
  locked_ids_.clear();
  selected_ids_.clear();
  identity_for_contact_.clear();
  search_query_ = "";
  step_ = kStepSelect;
  title_ = Tr("people_picker.title_call_add_guest").c_str();
  empty_hint_ = Tr("people_picker.empty").c_str();
  SyncCallAddGuestRows();
  UpdateCta();
  DirtyAll();
  PaneSpec spec;
  spec.key = "people_picker";
  layer_id_ = shell_navigation_.push_layer ? shell_navigation_.push_layer(spec) : -1;
  RegisterFlow();
  ShellDirty();
}

void PeoplePickerController::Open(PeoplePickerMode mode, std::unordered_set<std::string> locked_ids) {
  if (!MessagingInitialized()) {
    UserFeedback::Fail("Messaging not ready");
    ShellDirty();
    return;
  }

  Close();

  call_thread_id_.clear();
  call_id_.clear();
  call_video_ = false;
  identity_for_contact_.clear();
  mode_ = mode;
  locked_ids_ = std::move(locked_ids);
  selected_ids_ = locked_ids_;
  search_query_ = "";
  step_ = kStepSelect;
  group_title_ = Tr("people_picker.default_group_title").c_str();
  group_title_help_ = Tr("people_picker.group_title_help").c_str();
  pending_member_ids_.clear();
  member_summary_.clear();
  title_ = mode_ == PeoplePickerMode::FromDm ? Tr("people_picker.title_add").c_str()
                                             : Tr("people_picker.title").c_str();
  empty_hint_ = Tr("people_picker.empty").c_str();

  SyncRows();
  UpdateCta();
  DirtyAll();

  PaneSpec spec;
  spec.key = "people_picker";
  layer_id_ = shell_navigation_.push_layer ? shell_navigation_.push_layer(spec) : -1;
  RegisterFlow();
  ShellDirty();
}

void PeoplePickerController::RegisterFlow() {
  if (!flow_) {
    return;
  }
  flow_->BeginModal(
      layer_id_,
      [this]() {
        if (step_ == kStepName) {
          GoBackToSelect();
          return true;
        }
        return false;
      },
      [this]() { OnFlowDismissed(); });
}

void PeoplePickerController::Close() {
  if (flow_) {
    flow_->EndModal();
  }
  const int closing_id = layer_id_;
  layer_id_ = -1;
  ResetState();
  if (closing_id >= 0) {
    shell_navigation_.close_layer(closing_id);
  }
  DirtyAll();
}

void PeoplePickerController::OnFlowDismissed() {
  layer_id_ = -1;
  ResetState();
  DirtyAll();
  ShellDirty();
}

void PeoplePickerController::ResetState() {
  rows_.clear();
  member_summary_.clear();
  selected_ids_.clear();
  locked_ids_.clear();
  pending_member_ids_.clear();
  call_thread_id_.clear();
  call_id_.clear();
  call_video_ = false;
  identity_for_contact_.clear();
  search_query_ = "";
  step_ = kStepSelect;
  group_title_ = Tr("people_picker.default_group_title").c_str();
  group_title_help_ = Tr("people_picker.group_title_help").c_str();
  cta_enabled_ = false;
  empty_hint_ = Tr("people_picker.empty").c_str();
}

void PeoplePickerController::SyncRows() {
  rows_.clear();
  if (!MessagingInitialized() || !contacts_ports_.list_contacts) {
    return;
  }

  auto stored = contacts_ports_.list_contacts();
  if (!stored) {
    return;
  }

  std::string query = search_query_.c_str();
  std::transform(query.begin(), query.end(), query.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

  std::vector<Contact> candidates;
  candidates.reserve(stored->size());
  for (const Contact& contact : *stored) {
    const bool locked = locked_ids_.count(contact.id) > 0;
    if (!locked && !ContactIsSelectable(contact, MessagingReady() ? &contacts_ports_ : nullptr)) {
      continue;
    }
    if (!MatchesQuery(contact, query)) {
      continue;
    }
    candidates.push_back(contact);
  }

  for (const std::string& locked_id : locked_ids_) {
    const bool present =
        std::any_of(candidates.begin(), candidates.end(),
                    [&](const Contact& c) { return c.id == locked_id; });
    if (present) {
      continue;
    }
    if (contacts_ports_.get_contact) {
      auto contact = contacts_ports_.get_contact(locked_id);
      if (contact && *contact) {
        candidates.push_back(**contact);
      }
    }
  }

  std::sort(candidates.begin(), candidates.end(), [](const Contact& a, const Contact& b) {
    return FormatContactTitle(a) < FormatContactTitle(b);
  });

  rows_.reserve(candidates.size());
  for (const Contact& contact : candidates) {
    PickerRow row;
    row.id = contact.id.c_str();
    row.title = FormatContactTitle(contact).c_str();
    if (row.title.empty()) {
      row.title = Tr("people_picker.unnamed").c_str();
    }
    row.subtitle = ContactSubtitle(contact).c_str();
    row.locked = locked_ids_.count(contact.id) > 0;
    row.selected = selected_ids_.count(contact.id) > 0 || row.locked;
    if (row.locked) {
      selected_ids_.insert(contact.id);
    }
    rows_.push_back(std::move(row));
  }
}

void PeoplePickerController::SyncGroupCallRows() {
  rows_.clear();
  identity_for_contact_.clear();
  if (!MessagingInitialized() || call_thread_id_.empty() || !picker_ports_.get_thread ||
      !picker_ports_.list_group_roster) {
    return;
  }
  auto thread = picker_ports_.get_thread(call_thread_id_);
  if (!thread || !*thread || !(*thread)->group_id) {
    return;
  }
  auto roster = picker_ports_.list_group_roster(*(*thread)->group_id);
  if (!roster) {
    return;
  }
  std::string local_identity;
  if (picker_ports_.local_relay_identity) {
    if (auto identity = picker_ports_.local_relay_identity()) {
      local_identity = *identity;
    }
  }
  std::string query = search_query_.c_str();
  std::transform(query.begin(), query.end(), query.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

  struct RowCandidate {
    std::string identity;
    std::string title;
    std::string subtitle;
  };
  std::vector<RowCandidate> candidates;
  for (const GroupRosterMember& member : *roster) {
    if (member.member_identity.empty() || member.member_identity == local_identity) {
      continue;
    }
    const std::string& identity = member.member_identity;

    std::optional<Contact> contact;
    if (!member.contact_id.empty() && contacts_ports_.get_contact) {
      if (auto loaded = contacts_ports_.get_contact(member.contact_id); loaded && *loaded) {
        contact = **loaded;
      }
    }
    if (!contact && picker_ports_.find_contact_by_identity) {
      if (auto found = picker_ports_.find_contact_by_identity(identity, ContactIdKind::RelayUser); found && *found) {
        contact = **found;
      }
    }
    if (contact && contact->trust == TrustLevel::Blocked) {
      continue;
    }

    RowCandidate candidate;
    candidate.identity = identity;
    if (contact) {
      candidate.title = FormatContactTitle(*contact);
      candidate.subtitle = ContactSubtitle(*contact);
      if (!MatchesQuery(*contact, query) && !ContainsIgnoreCase(identity, query)) {
        continue;
      }
    } else {
      candidate.title = ShortRelayId(identity);
      if (!MatchesIdentityQuery(candidate.title, identity, query)) {
        continue;
      }
    }
    if (candidate.title.empty()) {
      candidate.title = Tr("people_picker.unnamed");
    }
    candidates.push_back(std::move(candidate));
  }
  std::sort(candidates.begin(), candidates.end(),
            [](const RowCandidate& a, const RowCandidate& b) { return a.title < b.title; });

  rows_.reserve(candidates.size());
  for (const RowCandidate& candidate : candidates) {
    PickerRow row;
    row.id = candidate.identity.c_str();
    row.title = candidate.title.c_str();
    row.subtitle = candidate.subtitle.c_str();
    row.locked = false;
    row.selected = selected_ids_.count(candidate.identity) > 0;
    identity_for_contact_[candidate.identity] = candidate.identity;
    rows_.push_back(std::move(row));
  }
}

void PeoplePickerController::SyncCallAddGuestRows() {
  rows_.clear();
  identity_for_contact_.clear();
  if (!MessagingInitialized() || call_id_.empty() || !call_ || !contacts_ports_.list_contacts ||
      !picker_ports_.list_call_participants) {
    return;
  }
  std::unordered_set<std::string> joined_identities;
  if (auto participants = picker_ports_.list_call_participants(call_id_)) {
    for (const CallParticipant& row : *participants) {
      joined_identities.insert(row.identity);
    }
  }

  auto stored = contacts_ports_.list_contacts();
  if (!stored) {
    return;
  }
  std::string query = search_query_.c_str();
  std::transform(query.begin(), query.end(), query.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

  std::vector<Contact> candidates;
  for (const Contact& contact : *stored) {
    if (!ContactIsSelectable(contact, MessagingReady() ? &contacts_ports_ : nullptr)) {
      continue;
    }
    const DirectChatTarget target = DirectChatTargetFromContact(contact, ThreadChannel::E2ePublic);
    if (target.peer_identity_value.empty()) {
      continue;
    }
    // Joined only — Left/Declined remain selectable for re-invite after eject/drop.
    if (joined_identities.count(target.peer_identity_value) > 0) {
      continue;
    }
    if (!MatchesQuery(contact, query)) {
      continue;
    }
    candidates.push_back(contact);
  }
  std::sort(candidates.begin(), candidates.end(), [](const Contact& a, const Contact& b) {
    return FormatContactTitle(a) < FormatContactTitle(b);
  });

  rows_.reserve(candidates.size());
  for (const Contact& contact : candidates) {
    const DirectChatTarget target = DirectChatTargetFromContact(contact, ThreadChannel::E2ePublic);
    PickerRow row;
    row.id = contact.id.c_str();
    row.title = FormatContactTitle(contact).c_str();
    if (row.title.empty()) {
      row.title = Tr("people_picker.unnamed").c_str();
    }
    row.subtitle = ContactSubtitle(contact).c_str();
    row.locked = false;
    row.selected = selected_ids_.count(contact.id) > 0;
    identity_for_contact_[contact.id] = target.peer_identity_value;
    rows_.push_back(std::move(row));
  }
}

void PeoplePickerController::UpdateCta() {
  const PeoplePickerCta cta = ComputePeoplePickerCta(mode_, FreeSelectedCount());
  cta_enabled_ = cta != PeoplePickerCta::Disabled;
  switch (cta) {
  case PeoplePickerCta::Message:
    cta_label_ = Tr("people_picker.cta_message").c_str();
    break;
  case PeoplePickerCta::CreateGroup:
    cta_label_ = Tr("people_picker.cta_next").c_str();
    break;
  case PeoplePickerCta::StartCall:
    if (mode_ == PeoplePickerMode::CallAddGuest) {
      cta_label_ = Tr("people_picker.cta_invite_to_call").c_str();
    } else {
      cta_label_ = call_video_ ? Tr("people_picker.cta_start_video_call").c_str()
                               : Tr("people_picker.cta_start_voice_call").c_str();
    }
    break;
  case PeoplePickerCta::Disabled:
  default:
    cta_label_ = Tr("people_picker.cta_select").c_str();
    break;
  }
}

int PeoplePickerController::FreeSelectedCount() const {
  int count = 0;
  for (const std::string& id : selected_ids_) {
    if (locked_ids_.count(id) == 0) {
      ++count;
    }
  }
  return count;
}

std::vector<std::string> PeoplePickerController::SelectedContactIds() const {
  std::vector<std::string> ids;
  ids.reserve(selected_ids_.size());
  for (const PickerRow& row : rows_) {
    if (selected_ids_.count(row.id.c_str()) > 0 || row.locked) {
      ids.push_back(row.id.c_str());
    }
  }
  for (const std::string& locked_id : locked_ids_) {
    if (std::find(ids.begin(), ids.end(), locked_id) == ids.end()) {
      ids.push_back(locked_id);
    }
  }
  for (const std::string& id : selected_ids_) {
    if (std::find(ids.begin(), ids.end(), id) == ids.end()) {
      ids.push_back(id);
    }
  }
  return ids;
}

std::vector<std::string> PeoplePickerController::SelectedInviteIdentities() const {
  std::vector<std::string> identities;
  for (const std::string& contact_id : SelectedContactIds()) {
    auto it = identity_for_contact_.find(contact_id);
    if (it != identity_for_contact_.end() && !it->second.empty()) {
      identities.push_back(it->second);
    }
  }
  return identities;
}

std::string PeoplePickerController::TitleForContactId(const std::string& contact_id) const {
  for (const PickerRow& row : rows_) {
    if (row.id.c_str() == contact_id) {
      return row.title.c_str();
    }
  }
  if (contacts_ports_.snapshot && contacts_ports_.snapshot().initialized && contacts_ports_.get_contact) {
    if (auto contact = contacts_ports_.get_contact(contact_id)) {
      if (*contact) {
        const std::string title = FormatContactTitle(**contact);
        return title.empty() ? Tr("people_picker.unnamed") : title;
      }
    }
  }
  return Tr("people_picker.unnamed");
}

std::string PeoplePickerController::TrimTitle(std::string title) const {
  while (!title.empty() && std::isspace(static_cast<unsigned char>(title.front()))) {
    title.erase(title.begin());
  }
  while (!title.empty() && std::isspace(static_cast<unsigned char>(title.back()))) {
    title.pop_back();
  }
  if (title.empty()) {
    title = Tr("people_picker.default_group_title");
  }
  return title;
}

void PeoplePickerController::DirtyAll() {
  auto& host = DataModelHost::Instance();
  host.Dirty("people_picker", "rows");
  host.Dirty("people_picker", "member_summary");
  host.Dirty("people_picker", "search_query");
  host.Dirty("people_picker", "title");
  host.Dirty("people_picker", "step");
  host.Dirty("people_picker", "group_title");
  host.Dirty("people_picker", "group_title_help");
  host.Dirty("people_picker", "cta_label");
  host.Dirty("people_picker", "cta_enabled");
  host.Dirty("people_picker", "empty_hint");
}

void PeoplePickerController::ToggleContactCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                                   const Rml::VariantList& args) {
  if (args.empty() || args[0].GetType() != Rml::Variant::STRING) {
    return;
  }
  Instance().OnToggleContact(std::string(args[0].Get<Rml::String>().c_str()));
}

void PeoplePickerController::OnSearchChangedCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                                     const Rml::VariantList& /*args*/) {
  Instance().OnSearchChanged();
}

void PeoplePickerController::ConfirmCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                             const Rml::VariantList& /*args*/) {
  Instance().OnConfirm();
}

void PeoplePickerController::CancelCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                            const Rml::VariantList& /*args*/) {
  Instance().OnCancel();
}

void PeoplePickerController::BackCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                          const Rml::VariantList& /*args*/) {
  Instance().OnBack();
}

void PeoplePickerController::CreateGroupCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                                   const Rml::VariantList& /*args*/) {
  Instance().OnCreateGroup();
}

void PeoplePickerController::OnToggleContact(const std::string& contact_id) {
  if (contact_id.empty() || locked_ids_.count(contact_id) > 0) {
    return;
  }
  if (selected_ids_.count(contact_id) > 0) {
    selected_ids_.erase(contact_id);
  } else {
    selected_ids_.insert(contact_id);
  }
  for (PickerRow& row : rows_) {
    if (row.id.c_str() == contact_id) {
      row.selected = selected_ids_.count(contact_id) > 0;
      break;
    }
  }
  UpdateCta();
  DirtyAll();
}

void PeoplePickerController::OnSearchChanged() {
  if (mode_ == PeoplePickerMode::GroupCall) {
    SyncGroupCallRows();
  } else if (mode_ == PeoplePickerMode::CallAddGuest) {
    SyncCallAddGuestRows();
  } else {
    SyncRows();
  }
  UpdateCta();
  DirtyAll();
}

void PeoplePickerController::OnCancel() {
  Close();
  ShellDirty();
}

void PeoplePickerController::OnBack() {
  if (step_ == kStepName) {
    GoBackToSelect();
    ShellDirty();
  }
}

void PeoplePickerController::OnConfirm() {
  if (!cta_enabled_) {
    return;
  }
  const PeoplePickerCta cta = ComputePeoplePickerCta(mode_, FreeSelectedCount());
  if (cta == PeoplePickerCta::StartCall) {
    OnStartCall();
    return;
  }
  const std::vector<std::string> ids = SelectedContactIds();
  if (cta == PeoplePickerCta::Message) {
    if (ids.size() != 1) {
      return;
    }
    StartDirectMessage(ids.front());
    return;
  }
  if (cta == PeoplePickerCta::CreateGroup) {
    if (ids.size() < 2) {
      return;
    }
    AdvanceToNameStep(ids);
    ShellDirty();
  }
}

void PeoplePickerController::OnStartCall() {
  if (!call_) {
    UserFeedback::Fail("Calls unavailable");
    return;
  }
  const std::vector<std::string> identities = SelectedInviteIdentities();
  if (identities.empty()) {
    UserFeedback::Fail("Select at least one person");
    return;
  }
  if (picker_ports_.list_call_participants) {
    size_t joined = 1;
    if (mode_ == PeoplePickerMode::CallAddGuest && !call_id_.empty()) {
      if (auto count = picker_ports_.list_call_participants(call_id_)) {
        joined = count->size();
      }
      if (!CallSessionLogic::CanAcceptJoin(joined + identities.size() - 1)) {
        UserFeedback::Fail("Call is full");
        return;
      }
    } else if (mode_ == PeoplePickerMode::GroupCall) {
      if (!CallSessionLogic::CanAcceptJoin(identities.size())) {
        UserFeedback::Fail("Too many invitees for this call");
        return;
      }
    }
  }
  // Close()/ResetState clears call_* fields — capture before dismissing the picker.
  const PeoplePickerMode mode = mode_;
  const std::string call_thread_id = call_thread_id_;
  const bool call_video = call_video_;
  Close();
  ShellDirty();
  if (mode == PeoplePickerMode::CallAddGuest) {
    call_->InviteIdentitiesToActiveCall(identities);
    return;
  }
  if (mode == PeoplePickerMode::GroupCall) {
    (void)call_->StartCallWithInvitees(call_thread_id, call_video, identities);
  }
}

void PeoplePickerController::AdvanceToNameStep(const std::vector<std::string>& member_contact_ids) {
  pending_member_ids_ = member_contact_ids;
  member_summary_.clear();
  member_summary_.reserve(member_contact_ids.size());
  for (const std::string& contact_id : member_contact_ids) {
    MemberSummaryRow row;
    row.title = TitleForContactId(contact_id).c_str();
    member_summary_.push_back(std::move(row));
  }
  step_ = kStepName;
  title_ = Tr("people_picker.group_title_prompt").c_str();
  group_title_ = Tr("people_picker.default_group_title").c_str();
  DirtyAll();
}

void PeoplePickerController::GoBackToSelect() {
  step_ = kStepSelect;
  pending_member_ids_.clear();
  member_summary_.clear();
  title_ = mode_ == PeoplePickerMode::FromDm ? Tr("people_picker.title_add").c_str()
                                             : Tr("people_picker.title").c_str();
  UpdateCta();
  DirtyAll();
}

void PeoplePickerController::OnCreateGroup() {
  if (pending_member_ids_.size() < 2) {
    return;
  }
  CreateGroupWithTitle(pending_member_ids_, group_title_.c_str());
}

void PeoplePickerController::FinishOpenThread() {
  Close();
  if (shell_navigation_.select_nav_tab) {
    shell_navigation_.select_nav_tab(NavTab::Sessions);
  }
  if (shell_navigation_.set_primary_pane) {
    shell_navigation_.set_primary_pane("chat");
  }
  if (chat_ports_.finalize_thread_display) {
    chat_ports_.finalize_thread_display();
  }
  ShellDirty();
}

void PeoplePickerController::StartDirectMessage(const std::string& contact_id) {
  if (!unlock_gate_) {
    return;
  }
  unlock_gate_->EnsureUnlocked([this, contact_id](const bool unlocked) {
    if (!unlocked) {
      ShowToast(Tr("people_picker.pin_required"));
      ShellDirty();
      return;
    }
    auto contact = contacts_ports_.get_contact
                       ? contacts_ports_.get_contact(contact_id)
                       : Roe<std::optional<Contact>>::error(Error("contacts port unavailable"));
    if (contact && *contact && contacts_ports_.register_contact_direct_endpoints) {
      contacts_ports_.register_contact_direct_endpoints(**contact);
    }
    auto thread = contacts_ports_.find_or_create_direct_thread
                      ? contacts_ports_.find_or_create_direct_thread(contact_id, ThreadChannel::E2e)
                      : Roe<Thread>::error(Error("contacts port unavailable"));
    if (!thread) {
      UserFeedback::Fail(thread.error().message);
      ShellDirty();
      return;
    }
    if (contacts_ports_.ensure_psk_generated) {
      (void)contacts_ports_.ensure_psk_generated(thread->id);
    }
    if (contacts_ports_.warm_peer_for_thread) {
      contacts_ports_.warm_peer_for_thread(thread->id);
    }
    FinishOpenThread();
  });
}

void PeoplePickerController::CreateGroupWithTitle(const std::vector<std::string>& member_contact_ids,
                                                  std::string title) {
  if (!unlock_gate_) {
    return;
  }
  unlock_gate_->EnsureUnlocked([this, member_contact_ids,
                                                title = TrimTitle(std::move(title))](const bool unlocked) {
    if (!unlocked) {
      ShowToast(Tr("people_picker.pin_required"));
      ShellDirty();
      return;
    }
    auto thread = picker_ports_.create_group ? picker_ports_.create_group(title, member_contact_ids)
                                             : Roe<Thread>::error(Error("picker port unavailable"));
    if (!thread) {
      UserFeedback::Fail(thread.error().message);
      ShellDirty();
      return;
    }
    FinishOpenThread();
  });
}

} // namespace pbr
