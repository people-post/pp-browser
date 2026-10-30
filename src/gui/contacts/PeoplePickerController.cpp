#include <stdexcept>
#include "gui/contacts/PeoplePickerController.h"

#include "foundation/error/AppError.h"
#include "foundation/i18n/LocalizationService.h"
#include "lib/calls/CallSessionLogic.h"
#include "lib/calls/CallTypes.h"
#include "lib/people/DirectChatTargetFromContact.h"
#include "lib/messaging/GroupTypes.h"
#include "common/thread/ThreadTypes.h"
#include "lib/people/ContactJson.h"
#include "lib/people/ContactTypes.h"
#include "lib/people/PeerDisplayLabel.h"
#include "lib/people/PeopleLookupFailure.h"
#include "lib/ui/ContextMenuHost.h"
#include "common/ValueJson.h"
#include "lib/ui/ShellTypes.h"
#include "lib/people/ContactReachability.h"
#include "gui/CallActionsPorts.h"
#include "gui/ChatSessionPorts.h"
#include "gui/shell/DataModelHost.h"
#include "gui/FlowCoordinatorPorts.h"
#include "gui/UnlockEnsurePorts.h"
#include "gui/UserFeedback.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <optional>
#include "common/PbrCompat.h"
#include "common/MeshAlias.h"

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
  if (target.peer_identity_kind == ContactIdKindToString(mesh::ContactIdKind::PeerId) && contact.multiaddrs.empty()) {
    return false;
  }
  return true;
}

std::string ContactSubtitle(const Contact& contact) {
  for (const mesh::ContactId& id : contact.ids) {
    if (id.kind == mesh::ContactIdKind::RelayUser && id.primary) {
      return ShortRelayId(id.value);
    }
  }
  for (const mesh::ContactId& id : contact.ids) {
    if (id.kind == mesh::ContactIdKind::RelayUser) {
      return ShortRelayId(id.value);
    }
  }
  for (const mesh::ContactId& id : contact.ids) {
    if (id.kind == mesh::ContactIdKind::PeerId) {
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
  for (const mesh::ContactId& id : contact.ids) {
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

PeoplePickerController* PeoplePickerController::installed_instance_ = nullptr;

void PeoplePickerController::InstallInstance(PeoplePickerController& controller) {
  installed_instance_ = &controller;
}

void PeoplePickerController::ClearInstance() {
  installed_instance_ = nullptr;
}

PeoplePickerController& PeoplePickerController::Instance() {
  if (!installed_instance_) {
    throw std::runtime_error("PeoplePickerController not installed");
  }
  return *installed_instance_;
}

PeoplePickerController::PeoplePickerController() {
  redirectLogger("PeoplePickerController");
}
void PeoplePickerController::BindContactsPorts(MessagingContactsPorts ports) {
  contacts_ports_ = std::move(ports);
}

void PeoplePickerController::BindPickerPorts(MessagingPeoplePickerPorts ports) {
  picker_ports_ = std::move(ports);
}

void PeoplePickerController::BindUnlockEnsure(UnlockEnsurePorts ports) {
  unlock_ensure_ = std::move(ports);
}

void PeoplePickerController::BindChatPorts(ChatSessionPorts ports) {
  chat_ports_ = std::move(ports);
}

void PeoplePickerController::BindFlowCoordinator(FlowCoordinatorPorts ports) {
  flow_coordinator_ = std::move(ports);
}

void PeoplePickerController::BindCallActions(CallActionsPorts ports) {
  call_actions_ = std::move(ports);
}

void PeoplePickerController::BindShellNavigation(ShellNavigationPorts ports) {
  shell_navigation_ = std::move(ports);
}

void PeoplePickerController::BindShellFeedback(ShellFeedbackPorts ports) {
  shell_feedback_ = std::move(ports);
}

void PeoplePickerController::BindSurfaceNotify(PeoplePickerSurfaceNotifyPorts ports) {
  surface_notify_ = std::move(ports);
}

PeoplePickerSurfaceSnapshot PeoplePickerController::BuildSurfaceSnapshot() const {
  PeoplePickerSurfaceSnapshot snap;
  snap.overlay_open = layer_id_ >= 0;
  return snap;
}

void PeoplePickerController::NotifySurfaceChanged() {
  if (surface_notify_.push_surface) {
    surface_notify_.push_surface(BuildSurfaceSnapshot());
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

bool PeoplePickerController::RegisterModel(ui::Context* context) {
  if (!context) {
    return false;
  }
  context_ = context;

  return DataModelHost::Instance().Register(context, "people_picker", [this](ui::DataModelConstructor& ctor) {
    auto& controller = *this;
    if (auto row_handle = ctor.RegisterStruct<PickerRow>()) {
      row_handle.RegisterMember("id", &PickerRow::id);
      row_handle.RegisterMember("title", &PickerRow::title);
      row_handle.RegisterMember("subtitle", &PickerRow::subtitle);
      row_handle.RegisterMember("selected", &PickerRow::selected);
      row_handle.RegisterMember("locked", &PickerRow::locked);
      row_handle.RegisterMember("disabled", &PickerRow::disabled);
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
    ctor.Bind("full_hint", &controller.full_hint_);
    ctor.Bind("invite_hint", &controller.invite_hint_);
    ctor.Bind("show_checks", &controller.show_checks_);
    ctor.Bind("show_cta", &controller.show_cta_);
    ctor.Bind("show_add_members", &controller.show_add_members_);
    ctor.Bind("show_call_video_option", &controller.show_call_video_option_);
    ctor.Bind("call_video_allowed", &controller.call_video_allowed_);
    ctor.BindEventCallback("toggle_contact", &PeoplePickerController::ToggleContactCallback);
    ctor.BindEventCallback("on_search_changed", &PeoplePickerController::OnSearchChangedCallback);
    ctor.BindEventCallback("confirm_picker", &PeoplePickerController::ConfirmCallback);
    ctor.BindEventCallback("cancel_picker", &PeoplePickerController::CancelCallback);
    ctor.BindEventCallback("back_picker", &PeoplePickerController::BackCallback);
    ctor.BindEventCallback("create_group", &PeoplePickerController::CreateGroupCallback);
    ctor.BindEventCallback("add_members", &PeoplePickerController::AddMembersCallback);
  });
}

void PeoplePickerController::OpenFree() {
  Open(PeoplePickerMode::Free, {});
}

void PeoplePickerController::OpenFromDm(const std::string& locked_contact_id) {
  if (locked_contact_id.empty()) {
    UserFeedback::Fail(Tr("people_picker.err.no_contact"));
    NotifySurfaceChanged();
    return;
  }
  Open(PeoplePickerMode::FromDm, {locked_contact_id});
}

void PeoplePickerController::OpenForGroupCall(const std::string& thread_id, const bool video_allowed) {
  if (!MessagingInitialized() || !picker_ports_.get_thread) {
    UserFeedback::Fail(Tr("people_picker.err.messaging_not_ready"));
    return;
  }
  auto thread = picker_ports_.get_thread(thread_id);
  if (!thread || !*thread || (*thread)->kind != ThreadKind::Group || !(*thread)->group_id) {
    UserFeedback::Fail(Tr("people_picker.err.group_required"));
    return;
  }
  Close();
  mode_ = PeoplePickerMode::GroupCall;
  call_thread_id_ = thread_id;
  call_id_.clear();
  // I1: carry the caller's voice/video menu choice into the picker's checkbox (still editable).
  call_video_allowed_ = video_allowed;
  show_call_video_option_ = true;
  locked_ids_.clear();
  selected_ids_.clear();
  identity_for_contact_.clear();
  search_query_ = "";
  step_ = kStepSelect;
  title_ = Tr("people_picker.title_group_call").c_str();
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
  NotifySurfaceChanged();
}

void PeoplePickerController::OpenForGroupMembers(const std::string& thread_id) {
  if (!MessagingInitialized() || !picker_ports_.get_thread || !picker_ports_.list_group_roster) {
    UserFeedback::Fail(Tr("people_picker.err.messaging_not_ready"));
    return;
  }
  auto thread = picker_ports_.get_thread(thread_id);
  if (!thread || !*thread || (*thread)->kind != ThreadKind::Group || !(*thread)->group_id) {
    UserFeedback::Fail(Tr("people_picker.err.group_required"));
    return;
  }
  Close();
  mode_ = PeoplePickerMode::GroupMembers;
  group_thread_id_ = thread_id;
  group_id_ = *(*thread)->group_id;
  group_sending_ = false;
  show_checks_ = false;
  show_cta_ = false;
  show_call_video_option_ = false;
  selected_ids_.clear();
  search_query_ = "";
  step_ = kStepSelect;
  title_ = Tr("people_picker.title_group_members").c_str();
  empty_hint_ = Tr("people_picker.empty_group_call").c_str();
  SyncGroupMemberRows();
  UpdateCta();
  DirtyAll();
  PaneSpec spec;
  spec.key = "people_picker";
  layer_id_ = shell_navigation_.push_layer ? shell_navigation_.push_layer(spec) : -1;
  RegisterFlow();
  NotifySurfaceChanged();
}

void PeoplePickerController::OpenForGroupAddMember() {
  const std::string group_id = group_id_;
  const std::string thread_id = group_thread_id_;
  Close();
  mode_ = PeoplePickerMode::GroupAddMember;
  group_id_ = group_id;
  group_thread_id_ = thread_id;
  group_sending_ = false;
  show_checks_ = true;
  show_cta_ = true;
  show_call_video_option_ = false;
  selected_ids_.clear();
  search_query_ = "";
  step_ = kStepSelect;
  title_ = Tr("people_picker.add_members").c_str();
  empty_hint_ = Tr("people_picker.empty_group_add").c_str();
  SyncGroupAddRows();
  UpdateCta();
  DirtyAll();
  PaneSpec spec;
  spec.key = "people_picker";
  layer_id_ = shell_navigation_.push_layer ? shell_navigation_.push_layer(spec) : -1;
  RegisterFlow();
  NotifySurfaceChanged();
}

void PeoplePickerController::OpenForCallAddGuest(const std::string& call_id) {
  if (!MessagingInitialized() || call_id.empty()) {
    UserFeedback::Fail(Tr("people_picker.err.calls_unavailable"));
    return;
  }
  Close();
  mode_ = PeoplePickerMode::CallAddGuest;
  call_thread_id_.clear();
  call_id_ = call_id;
  call_video_allowed_ = false;
  show_call_video_option_ = false;
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
  NotifySurfaceChanged();
}

void PeoplePickerController::Open(PeoplePickerMode mode, std::unordered_set<std::string> locked_ids) {
  if (!MessagingInitialized()) {
    UserFeedback::Fail(Tr("people_picker.err.messaging_not_ready"));
    NotifySurfaceChanged();
    return;
  }

  Close();

  call_thread_id_.clear();
  call_id_.clear();
  call_video_allowed_ = false;
  show_call_video_option_ = false;
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
  NotifySurfaceChanged();
}

void PeoplePickerController::RegisterFlow() {
  if (!flow_coordinator_.begin_modal) {
    return;
  }
  flow_coordinator_.begin_modal(
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
  if (flow_coordinator_.end_modal) {
    flow_coordinator_.end_modal();
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
  NotifySurfaceChanged();
}

void PeoplePickerController::ResetState() {
  rows_.clear();
  member_summary_.clear();
  selected_ids_.clear();
  locked_ids_.clear();
  pending_member_ids_.clear();
  call_thread_id_.clear();
  call_id_.clear();
  call_video_allowed_ = false;
  show_call_video_option_ = false;
  identity_for_contact_.clear();
  group_id_.clear();
  group_thread_id_.clear();
  group_is_owner_ = false;
  group_sending_ = false;
  group_roster_count_ = 0;
  member_rows_.clear();
  show_checks_ = true;
  show_cta_ = true;
  show_add_members_ = false;
  full_hint_ = "";
  invite_hint_ = "";
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
      if (auto found = picker_ports_.find_contact_by_identity(identity, mesh::ContactIdKind::Account); found && *found) {
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

std::optional<Contact> PeoplePickerController::ContactForMember(const GroupRosterMember& member) const {
  if (!member.contact_id.empty() && contacts_ports_.get_contact) {
    if (auto loaded = contacts_ports_.get_contact(member.contact_id); loaded && *loaded) {
      return **loaded;
    }
  }
  if (picker_ports_.find_contact_by_identity) {
    if (auto found = picker_ports_.find_contact_by_identity(member.member_identity, mesh::ContactIdKind::Account);
        found && *found) {
      return **found;
    }
  }
  return std::nullopt;
}

void PeoplePickerController::SyncGroupMemberRows() {
  rows_.clear();
  member_rows_.clear();
  show_add_members_ = false;
  if (!MessagingInitialized() || group_id_.empty() || !picker_ports_.list_group_roster) {
    return;
  }
  auto roster = picker_ports_.list_group_roster(group_id_);
  if (!roster) {
    return;
  }
  group_roster_count_ = static_cast<int>(roster->size());
  std::string local_identity;
  if (picker_ports_.local_relay_identity) {
    if (auto identity = picker_ports_.local_relay_identity()) {
      local_identity = *identity;
    }
  }
  local_identity_ = local_identity;
  group_is_owner_ = std::any_of(roster->begin(), roster->end(), [&](const GroupRosterMember& member) {
    return member.member_identity == local_identity && member.role == MemberRole::Owner;
  });
  show_add_members_ = group_is_owner_;
  std::unordered_set<std::string> unreachable;
  if (picker_ports_.list_unreachable_members) {
    for (const std::string& identity : picker_ports_.list_unreachable_members(group_id_)) {
      unreachable.insert(identity);
    }
  }
  std::string query = search_query_.c_str();
  std::transform(query.begin(), query.end(), query.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

  std::vector<GroupMemberRow> members;
  std::optional<GroupMemberRow> self_row;
  for (const GroupRosterMember& member : *roster) {
    if (member.member_identity.empty()) {
      continue;
    }
    if (member.member_identity == local_identity) {
      GroupMemberRow self = BuildGroupMemberRow(member, std::nullopt, false);
      self.title = Tr("people_picker.you");
      if (MatchesIdentityQuery(self.title, self.identity, query)) {
        self_row = std::move(self);
      }
      continue;
    }
    const std::optional<Contact> contact = ContactForMember(member);
    const std::string card_nickname = !contact && picker_ports_.kept_card_nickname
                                          ? picker_ports_.kept_card_nickname(member.member_identity)
                                          : std::string();
    GroupMemberRow row =
        BuildGroupMemberRow(member, contact, unreachable.count(member.member_identity) > 0, card_nickname);
    if (!MatchesIdentityQuery(row.title, row.identity, query)) {
      continue;
    }
    members.push_back(std::move(row));
  }
  SortGroupMemberRows(members);
  // This device's own row first, so the list counts the same people as "N members".
  if (self_row) {
    members.insert(members.begin(), std::move(*self_row));
  }
  rows_.reserve(members.size());
  for (GroupMemberRow& member : members) {
    PickerRow row;
    row.id = member.identity.c_str();
    row.title = member.title.c_str();
    std::string subtitle;
    if (member.is_owner) {
      subtitle = Tr("people_picker.badge_owner");
    }
    if (member.unreachable) {
      subtitle += (subtitle.empty() ? "" : " · ") + Tr("people_picker.badge_unreachable");
    }
    row.subtitle = subtitle.c_str();
    rows_.push_back(std::move(row));
    member_rows_[member.identity] = std::move(member);
  }
  // Invites this device sent that nobody has accepted yet: below the members, never counted as members.
  if (group_is_owner_ && picker_ports_.list_invited_waiting) {
    std::unordered_set<std::string> roster_identities;
    for (const GroupRosterMember& member : *roster) {
      roster_identities.insert(member.member_identity);
    }
    const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::system_clock::now().time_since_epoch())
                               .count();
    std::vector<GroupMemberRow> invited;
    for (const PendingGroupInvite& invite : picker_ports_.list_invited_waiting(group_id_)) {
      if (invite.invitee_identity.empty() || invite.invitee_identity == local_identity) {
        continue;
      }
      GroupRosterMember as_member;
      as_member.member_identity = invite.invitee_identity;
      const std::optional<Contact> contact = ContactForMember(as_member);
      const std::string card_nickname = !contact && picker_ports_.kept_card_nickname
                                            ? picker_ports_.kept_card_nickname(invite.invitee_identity)
                                            : std::string();
      std::optional<GroupMemberRow> row = BuildInvitedRow(invite, roster_identities, contact, now_ms, card_nickname);
      if (row && MatchesIdentityQuery(row->title, row->identity, query)) {
        invited.push_back(std::move(*row));
      }
    }
    std::stable_sort(invited.begin(), invited.end(),
                     [](const GroupMemberRow& a, const GroupMemberRow& b) { return a.title < b.title; });
    for (GroupMemberRow& member : invited) {
      PickerRow row;
      row.id = member.identity.c_str();
      row.title = member.title.c_str();
      row.subtitle = Tr(member.invited_stale ? "people_picker.invited_stale" : "people_picker.invited_waiting").c_str();
      rows_.push_back(std::move(row));
      member_rows_[member.identity] = std::move(member);
    }
  }
}

void PeoplePickerController::SyncGroupAddRows() {
  rows_.clear();
  member_rows_.clear();
  show_add_members_ = false;
  if (!MessagingInitialized() || group_id_.empty() || !contacts_ports_.list_contacts ||
      !picker_ports_.list_group_roster) {
    return;
  }
  auto roster = picker_ports_.list_group_roster(group_id_);
  auto stored = contacts_ports_.list_contacts();
  if (!roster || !stored) {
    return;
  }
  group_roster_count_ = static_cast<int>(roster->size());
  std::string local_identity;
  if (picker_ports_.local_relay_identity) {
    if (auto identity = picker_ports_.local_relay_identity()) {
      local_identity = *identity;
    }
  }
  std::unordered_set<std::string> roster_identities;
  for (const GroupRosterMember& member : *roster) {
    roster_identities.insert(member.member_identity);
  }
  std::string query = search_query_.c_str();
  std::transform(query.begin(), query.end(), query.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

  std::vector<Contact> selectable;
  std::vector<AddMemberCandidate> candidates;
  for (const Contact& contact : *stored) {
    if (!ContactIsSelectable(contact, MessagingReady() ? &contacts_ports_ : nullptr) || !MatchesQuery(contact, query)) {
      continue;
    }
    candidates.push_back({contact.id, DirectChatTargetFromContact(contact, ThreadChannel::E2ePublic).peer_identity_value});
    selectable.push_back(contact);
  }
  const std::vector<std::string> ids = AddMemberCandidateIds(candidates, roster_identities, local_identity);
  std::vector<Contact> shown;
  for (const Contact& contact : selectable) {
    if (std::find(ids.begin(), ids.end(), contact.id) != ids.end()) {
      shown.push_back(contact);
    }
  }
  std::sort(shown.begin(), shown.end(), [](const Contact& a, const Contact& b) {
    return FormatContactTitle(a) < FormatContactTitle(b);
  });
  rows_.reserve(shown.size());
  for (const Contact& contact : shown) {
    PickerRow row;
    row.id = contact.id.c_str();
    row.title = FormatContactTitle(contact).c_str();
    if (row.title.empty()) {
      row.title = Tr("people_picker.unnamed").c_str();
    }
    row.subtitle = ContactSubtitle(contact).c_str();
    row.selected = selected_ids_.count(contact.id) > 0;
    rows_.push_back(std::move(row));
  }
}

void PeoplePickerController::RefreshGroupAddLimit() {
  const int selected = FreeSelectedCount();
  for (PickerRow& row : rows_) {
    row.disabled = GroupAddRowDisabled(group_roster_count_, selected, row.selected);
  }
  full_hint_ = GroupIsFull(group_roster_count_, selected)
                   ? Tr("people_picker.group_full", {{"max", std::to_string(kGroupMemberLimit)}}).c_str()
                   : "";
}

void PeoplePickerController::SyncCallAddGuestRows() {
  rows_.clear();
  identity_for_contact_.clear();
  if (!MessagingInitialized() || call_id_.empty() || !call_actions_.invite_identities ||
      !contacts_ports_.list_contacts || !picker_ports_.list_call_participants) {
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
  const PeoplePickerCta cta = ComputePeoplePickerCta(mode_, FreeSelectedCount(), group_sending_);
  cta_enabled_ = cta != PeoplePickerCta::Disabled && cta != PeoplePickerCta::Inviting;
  invite_hint_ = ShowInviteNeedsContactHint(cta) ? Tr("people_picker.invite_needs_contact").c_str() : "";
  if (mode_ == PeoplePickerMode::GroupAddMember) {
    RefreshGroupAddLimit();
  }
  switch (cta) {
  case PeoplePickerCta::Invite:
    cta_label_ = Tr("people_picker.cta_invite", {{"count", std::to_string(FreeSelectedCount())}}).c_str();
    break;
  case PeoplePickerCta::Inviting:
    cta_label_ = Tr("people_picker.cta_inviting").c_str();
    break;
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
      cta_label_ = Tr("people_picker.cta_start_call").c_str();
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
  host.Dirty("people_picker", "full_hint");
  host.Dirty("people_picker", "invite_hint");
  host.Dirty("people_picker", "show_checks");
  host.Dirty("people_picker", "show_cta");
  host.Dirty("people_picker", "show_add_members");
}

void PeoplePickerController::ToggleContactCallback(ui::DataModelHandle /*model*/, ui::Event& ev,
                                                   const ui::VariantList& args) {
  if (args.empty() || args[0].GetType() != ui::Variant::STRING) {
    return;
  }
  PeoplePickerController& self = Instance();
  const std::string id(args[0].Get<ui::String>().c_str());
  if (self.mode_ == PeoplePickerMode::GroupMembers) {
    self.OnMemberTapped(id, MenuPositionBelowEvent(ev));
    return;
  }
  self.OnToggleContact(id);
}

void PeoplePickerController::AddMembersCallback(ui::DataModelHandle /*model*/, ui::Event& /*ev*/,
                                                const ui::VariantList& /*args*/) {
  Instance().OpenForGroupAddMember();
}

void PeoplePickerController::OnSearchChangedCallback(ui::DataModelHandle /*model*/, ui::Event& /*ev*/,
                                                     const ui::VariantList& /*args*/) {
  Instance().OnSearchChanged();
}

void PeoplePickerController::ConfirmCallback(ui::DataModelHandle /*model*/, ui::Event& /*ev*/,
                                             const ui::VariantList& /*args*/) {
  Instance().OnConfirm();
}

void PeoplePickerController::CancelCallback(ui::DataModelHandle /*model*/, ui::Event& /*ev*/,
                                            const ui::VariantList& /*args*/) {
  Instance().OnCancel();
}

void PeoplePickerController::BackCallback(ui::DataModelHandle /*model*/, ui::Event& /*ev*/,
                                          const ui::VariantList& /*args*/) {
  Instance().OnBack();
}

void PeoplePickerController::CreateGroupCallback(ui::DataModelHandle /*model*/, ui::Event& /*ev*/,
                                                   const ui::VariantList& /*args*/) {
  Instance().OnCreateGroup();
}

void PeoplePickerController::OnToggleContact(const std::string& contact_id) {
  if (contact_id.empty() || locked_ids_.count(contact_id) > 0 || group_sending_) {
    return;
  }
  if (selected_ids_.count(contact_id) > 0) {
    selected_ids_.erase(contact_id);
  } else {
    if (mode_ == PeoplePickerMode::GroupAddMember &&
        GroupAddRowDisabled(group_roster_count_, FreeSelectedCount(), false)) {
      return;
    }
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
  } else if (mode_ == PeoplePickerMode::GroupMembers) {
    SyncGroupMemberRows();
  } else if (mode_ == PeoplePickerMode::GroupAddMember) {
    SyncGroupAddRows();
  } else {
    SyncRows();
  }
  UpdateCta();
  DirtyAll();
}

void PeoplePickerController::OnCancel() {
  Close();
  NotifySurfaceChanged();
}

void PeoplePickerController::OnBack() {
  if (step_ == kStepName) {
    GoBackToSelect();
    NotifySurfaceChanged();
  }
}

void PeoplePickerController::OnConfirm() {
  if (!cta_enabled_) {
    return;
  }
  const PeoplePickerCta cta = ComputePeoplePickerCta(mode_, FreeSelectedCount(), group_sending_);
  if (cta == PeoplePickerCta::Invite) {
    OnInviteMembers();
    return;
  }
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
    NotifySurfaceChanged();
  }
}

void PeoplePickerController::OnStartCall() {
  if (!call_actions_.invite_identities && !call_actions_.start_with_invitees) {
    UserFeedback::Fail(Tr("people_picker.err.calls_unavailable"));
    return;
  }
  const std::vector<std::string> identities = SelectedInviteIdentities();
  if (identities.empty()) {
    UserFeedback::Fail(Tr("people_picker.err.select_one"));
    return;
  }
  if (picker_ports_.list_call_participants) {
    size_t joined = 1;
    if (mode_ == PeoplePickerMode::CallAddGuest && !call_id_.empty()) {
      if (auto count = picker_ports_.list_call_participants(call_id_)) {
        joined = count->size();
      }
      if (!CallSessionLogic::CanAcceptJoin(joined + identities.size() - 1)) {
        UserFeedback::Fail(Tr("people_picker.err.call_full"));
        return;
      }
    } else if (mode_ == PeoplePickerMode::GroupCall) {
      if (!CallSessionLogic::CanAcceptJoin(identities.size())) {
        UserFeedback::Fail(Tr("people_picker.err.too_many"));
        return;
      }
    }
  }
  // Close()/ResetState clears call_* fields — capture before dismissing the picker.
  const PeoplePickerMode mode = mode_;
  const std::string call_thread_id = call_thread_id_;
  const bool call_video_allowed = call_video_allowed_;
  Close();
  NotifySurfaceChanged();
  if (mode == PeoplePickerMode::CallAddGuest) {
    if (call_actions_.invite_identities) {
      call_actions_.invite_identities(identities);
    }
    return;
  }
  if (mode == PeoplePickerMode::GroupCall) {
    if (call_actions_.start_with_invitees) {
      (void)call_actions_.start_with_invitees(call_thread_id, call_video_allowed, identities);
    }
  }
}

void PeoplePickerController::OnInviteMembers() {
  if (group_sending_ || group_id_.empty() || !picker_ports_.invite_members_async) {
    return;
  }
  // Everyone picked, also those a later search hides.
  std::vector<std::string> contact_ids(selected_ids_.begin(), selected_ids_.end());
  std::unordered_map<std::string, std::string> names;
  for (const std::string& contact_id : contact_ids) {
    if (contacts_ports_.get_contact) {
      if (auto loaded = contacts_ports_.get_contact(contact_id); loaded && *loaded) {
        names[contact_id] = FormatContactTitle(**loaded);
      }
    }
  }
  if (contact_ids.empty()) {
    return;
  }
  group_sending_ = true;
  const uint64_t batch = ++invite_batch_;
  UpdateCta();
  DirtyAll();
  const std::string group_id = group_id_;
  picker_ports_.invite_members_async(group_id, std::move(contact_ids),
                                     [this, batch, group_id, names](GroupInviteBatchResult result) {
                                       OnInviteFinished(batch, group_id, names, result);
                                     });
}

void PeoplePickerController::OnInviteFinished(const uint64_t batch, const std::string& group_id,
                                              const std::unordered_map<std::string, std::string>& names,
                                              const GroupInviteBatchResult& result) {
  const bool still_here = layer_id_ >= 0 && mode_ == PeoplePickerMode::GroupAddMember &&
                          group_id_ == group_id && invite_batch_ == batch;
  if (still_here) {
    group_sending_ = false;
  }
  if (result.failed.empty()) {
    ShowToast(Tr("people_picker.invite_sent", {{"count", std::to_string(result.sent.size())}}));
  } else {
    std::string failed_names;
    for (const auto& [contact_id, reason] : result.failed) {
      if (!failed_names.empty()) {
        failed_names += ", ";
      }
      auto it = names.find(contact_id);
      failed_names += it != names.end() ? it->second : Tr("people_picker.unnamed");
      log().warning << "Group invite not sent: " << reason;
    }
    ShowToast(Tr("people_picker.invite_partial",
                 {{"ok", std::to_string(result.sent.size())},
                  {"failed", std::to_string(result.failed.size())},
                  {"names", failed_names}}),
              ToastDuration::Long);
  }
  if (still_here) {
    Close();
  }
  NotifySurfaceChanged();
}

void PeoplePickerController::OnMemberTapped(const std::string& identity, const ui::Vector2i menu_position) {
  auto found = member_rows_.find(identity);
  if (found == member_rows_.end()) {
    return;
  }
  const GroupMemberRow row = found->second;
  if (row.identity == local_identity_) {
    return;
  }
  // A non-owner who taps a contact goes straight to the chat with them.
  if (!group_is_owner_ && row.IsContact()) {
    StartDirectMessage(row.contact_id);
    return;
  }
  std::vector<ContextMenuAction> actions;
  if (row.IsContact()) {
    actions.push_back({"member_message", Tr("people_picker.cta_message"), nullptr,
                       [this, contact_id = row.contact_id]() { StartDirectMessage(contact_id); },
                       "../icons/message.svg"});
  } else {
    actions.push_back({"member_add_contact", Tr("people_picker.add_as_contact"), nullptr,
                       [this, row]() { AddMemberAsContact(row); }, "../icons/contacts.svg"});
  }
  if (group_is_owner_) {
    if (row.IsContact()) {
      actions.push_back({"member_reinvite", Tr("people_picker.reinvite"), nullptr,
                         [this, row]() { ReinviteMember(row); },
                         "../icons/group.svg"});
    }
    if (!row.invited) {
      actions.push_back({"member_remove", Tr("people_picker.remove_from_group"), nullptr,
                         [this, row]() { RemoveMember(row); }, "../icons/trash.svg", true});
    }
  }
  ContextMenuHost::Instance().ShowActions(menu_position, std::move(actions));
}

void PeoplePickerController::ReinviteMember(const GroupMemberRow& row) {
  if (group_id_.empty() || !picker_ports_.invite_members_async) {
    return;
  }
  picker_ports_.invite_members_async(group_id_, {row.contact_id}, [this, name = row.title](GroupInviteBatchResult result) {
    if (!result.failed.empty()) {
      log().warning << "Group re-invite not sent: " << result.failed.front().second;
      ShowToast(Tr("people_picker.invite_partial", {{"ok", "0"}, {"failed", "1"}, {"names", name}}),
                ToastDuration::Long);
      return;
    }
    ShowToast(Tr("people_picker.reinvite_sent"), ToastDuration::Long);
  });
}

void PeoplePickerController::RemoveMember(const GroupMemberRow& row) {
  if (!shell_feedback_.show_confirm) {
    return;
  }
  shell_feedback_.show_confirm(
      Tr("chat.group.remove_member_title"), Tr("chat.group.remove_member_confirm", {{"name", row.title}}),
      [this, group_id = group_id_, identity = row.identity](const bool ok) {
        if (!ok || group_id.empty() || !picker_ports_.remove_member_by_identity) {
          return;
        }
        if (auto removed = picker_ports_.remove_member_by_identity(group_id, identity); !removed) {
          UserFeedback::Fail(removed.error().message);
          return;
        }
        if (contacts_ports_.notify_thread_changed) {
          contacts_ports_.notify_thread_changed();
        }
        SyncGroupMemberRows();
        DirtyAll();
        NotifySurfaceChanged();
      },
      {});
}

void PeoplePickerController::AddMemberAsContact(const GroupMemberRow& row) {
  if (!picker_ports_.dispatch_action_async) {
    return;
  }
  // Keys come from the client's own directory lookup for this account id, never from the group.
  Object payload;
  payload.set("type", "add_contact");
  payload.set("account_id", row.identity);
  picker_ports_.dispatch_action_async(
      DumpJson(payload), [this, name = row.title](Roe<std::optional<std::string>> added) {
        if (!added) {
          switch (ClassifyPeopleLookupError(added.error())) {
          case PeopleLookupFailure::NotFound:
            UserFeedback::Fail(Tr("contacts.err.user_not_found"));
            break;
          case PeopleLookupFailure::DirectoryUnreachable:
            UserFeedback::Fail(Tr("contacts.err.directory_unreachable"));
            break;
          case PeopleLookupFailure::Other:
            log().warning << "Add member as contact failed: " << AppError::Log(added.error());
            UserFeedback::Fail(Tr("contacts.err.add"));
            break;
          }
          return;
        }
        ShowToast(Tr("contacts.added", {{"name", name}}));
        if (layer_id_ >= 0 && mode_ == PeoplePickerMode::GroupMembers) {
          SyncGroupMemberRows();
          DirtyAll();
        }
        NotifySurfaceChanged();
      });
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
  NotifySurfaceChanged();
}

void PeoplePickerController::StartDirectMessage(const std::string& contact_id) {
  if (!unlock_ensure_.ensure_unlocked) {
    return;
  }
  unlock_ensure_.ensure_unlocked([this, contact_id](const bool unlocked) {
    if (!unlocked) {
      ShowToast(Tr("people_picker.pin_required"));
      NotifySurfaceChanged();
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
      NotifySurfaceChanged();
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
  if (!unlock_ensure_.ensure_unlocked) {
    return;
  }
  unlock_ensure_.ensure_unlocked([this, member_contact_ids,
                                                title = TrimTitle(std::move(title))](const bool unlocked) {
    if (!unlocked) {
      ShowToast(Tr("people_picker.pin_required"));
      NotifySurfaceChanged();
      return;
    }
    auto thread = picker_ports_.create_group ? picker_ports_.create_group(title, member_contact_ids)
                                             : Roe<Thread>::error(Error("picker port unavailable"));
    if (!thread) {
      UserFeedback::Fail(thread.error().message);
      NotifySurfaceChanged();
      return;
    }
    FinishOpenThread();
  });
}

} // namespace pbr
