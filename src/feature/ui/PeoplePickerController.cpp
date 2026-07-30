#include <stdexcept>
#include "feature/ui/PeoplePickerController.h"

#include "base/i18n/LocalizationService.h"
#include "base/messaging/DirectChatTarget.h"
#include "base/people/ContactJson.h"
#include "base/people/ContactTypes.h"
#include "base/people/PeerDisplayLabel.h"
#include "base/ui/ShellTypes.h"
#include "feature/messaging/MessagingHub.h"
#include "feature/ui/ChatSessionPorts.h"
#include "feature/ui/DataModelHost.h"
#include "feature/ui/FlowCoordinator.h"
#include "feature/ui/PinGateController.h"
#include "feature/ui/ShellFeedback.h"
#include "feature/ui/ShellHost.h"
#include "feature/ui/UserFeedback.h"

#include <algorithm>
#include <cctype>

namespace pbr {
namespace {

constexpr const char* kStepSelect = "select";
constexpr const char* kStepName = "name";

bool ContactIsMessageable(const Contact& contact) {
  if (contact.trust == TrustLevel::Blocked) {
    return false;
  }
  const DirectChatTarget target = DirectChatTargetFromContact(contact, ThreadChannel::E2ePublic);
  if (target.peer_identity_value.empty()) {
    return false;
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

bool MatchesQuery(const Contact& contact, const std::string& query_lower) {
  if (query_lower.empty()) {
    return true;
  }
  auto contains = [&](const std::string& hay) {
    std::string lower = hay;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lower.find(query_lower) != std::string::npos;
  };
  if (contains(contact.display_name) || contains(contact.server_nickname)) {
    return true;
  }
  for (const ContactId& id : contact.ids) {
    if (contains(id.value)) {
      return true;
    }
  }
  return false;
}

} // namespace

PeoplePickerController::PeoplePickerController() {
  redirectLogger("PeoplePickerController");
}

PeoplePickerController& PeoplePickerController::Instance() {
  static PeoplePickerController controller;
  return controller;
}
void PeoplePickerController::BindMessaging(MessagingHub& messaging) {
  messaging_ = &messaging;
}

void PeoplePickerController::BindChatPorts(ChatSessionPorts ports) {
  chat_ports_ = std::move(ports);
}

MessagingHub& PeoplePickerController::Hub() {
  if (!messaging_) {
    throw std::runtime_error("PeoplePickerController messaging not bound");
  }
  return *messaging_;
}

const MessagingHub& PeoplePickerController::Hub() const {
  if (!messaging_) {
    throw std::runtime_error("PeoplePickerController messaging not bound");
  }
  return *messaging_;
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
    ShellHost::Instance().DirtyWindow();
    return;
  }
  Open(PeoplePickerMode::FromDm, {locked_contact_id});
}

void PeoplePickerController::Open(PeoplePickerMode mode, std::unordered_set<std::string> locked_ids) {
  if (!Instance().Hub().IsInitialized()) {
    UserFeedback::Fail("Messaging not ready");
    ShellHost::Instance().DirtyWindow();
    return;
  }

  Close();

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

  SyncRows();
  UpdateCta();
  DirtyAll();

  PaneSpec spec;
  spec.key = "people_picker";
  layer_id_ = ShellHost::Instance().PushLayer(spec);
  RegisterFlow();
  ShellHost::Instance().DirtyWindow();
}

void PeoplePickerController::RegisterFlow() {
  FlowCoordinator::Instance().BeginModal(
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
  FlowCoordinator::Instance().EndModal();
  const int closing_id = layer_id_;
  layer_id_ = -1;
  ResetState();
  if (closing_id >= 0) {
    ShellHost::Instance().CloseLayer(closing_id);
  }
  DirtyAll();
}

void PeoplePickerController::OnFlowDismissed() {
  layer_id_ = -1;
  ResetState();
  DirtyAll();
  ShellHost::Instance().DirtyWindow();
}

void PeoplePickerController::ResetState() {
  rows_.clear();
  member_summary_.clear();
  selected_ids_.clear();
  locked_ids_.clear();
  pending_member_ids_.clear();
  search_query_ = "";
  step_ = kStepSelect;
  group_title_ = Tr("people_picker.default_group_title").c_str();
  group_title_help_ = Tr("people_picker.group_title_help").c_str();
  cta_enabled_ = false;
}

void PeoplePickerController::SyncRows() {
  rows_.clear();
  if (!Instance().Hub().IsInitialized()) {
    return;
  }

  auto stored = Instance().Hub().Contacts().List();
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
    if (!locked && !ContactIsMessageable(contact)) {
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
    auto contact = Instance().Hub().Contacts().Get(locked_id);
    if (contact && *contact) {
      candidates.push_back(**contact);
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

std::string PeoplePickerController::TitleForContactId(const std::string& contact_id) const {
  for (const PickerRow& row : rows_) {
    if (row.id.c_str() == contact_id) {
      return row.title.c_str();
    }
  }
  if (Instance().Hub().IsInitialized()) {
    if (auto contact = Instance().Hub().Contacts().Get(contact_id)) {
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
  SyncRows();
  UpdateCta();
  DirtyAll();
}

void PeoplePickerController::OnCancel() {
  Close();
  ShellHost::Instance().DirtyWindow();
}

void PeoplePickerController::OnBack() {
  if (step_ == kStepName) {
    GoBackToSelect();
    ShellHost::Instance().DirtyWindow();
  }
}

void PeoplePickerController::OnConfirm() {
  if (!cta_enabled_) {
    return;
  }
  const PeoplePickerCta cta = ComputePeoplePickerCta(mode_, FreeSelectedCount());
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
    ShellHost::Instance().DirtyWindow();
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
  ShellHost::Instance().SelectNavTab(NavTab::Sessions);
  ShellHost::Instance().SetPrimaryPane("chat");
  if (chat_ports_.finalize_thread_display) {
    chat_ports_.finalize_thread_display();
  }
  ShellHost::Instance().DirtyWindow();
}

void PeoplePickerController::StartDirectMessage(const std::string& contact_id) {
  PinGateController::Instance().EnsureUnlocked([this, contact_id](const bool unlocked) {
    if (!unlocked) {
      ShellFeedback::ShowToast(ShellHost::Instance().State(), Tr("people_picker.pin_required"));
      ShellHost::Instance().DirtyWindow();
      return;
    }
    auto contact = Instance().Hub().Contacts().Get(contact_id);
    if (contact && *contact) {
      Instance().Hub().P2p().RegisterContactDirectEndpoints(**contact);
    }
    auto thread = Instance().Hub().Inbox().FindOrCreateDirectThread(contact_id, ThreadChannel::E2e);
    if (!thread) {
      UserFeedback::Fail(thread.error().message);
      ShellHost::Instance().DirtyWindow();
      return;
    }
    (void)Instance().Hub().P2p().EnsurePskGenerated(thread->id);
    Instance().Hub().P2p().WarmPeerForThread(thread->id);
    FinishOpenThread();
  });
}

void PeoplePickerController::CreateGroupWithTitle(const std::vector<std::string>& member_contact_ids,
                                                  std::string title) {
  PinGateController::Instance().EnsureUnlocked([this, member_contact_ids,
                                                title = TrimTitle(std::move(title))](const bool unlocked) {
    if (!unlocked) {
      ShellFeedback::ShowToast(ShellHost::Instance().State(), Tr("people_picker.pin_required"));
      ShellHost::Instance().DirtyWindow();
      return;
    }
    auto thread = Instance().Hub().Inbox().CreateGroup(title, member_contact_ids);
    if (!thread) {
      UserFeedback::Fail(thread.error().message);
      ShellHost::Instance().DirtyWindow();
      return;
    }
    FinishOpenThread();
  });
}

} // namespace pbr
