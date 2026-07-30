#include <stdexcept>
#include "feature/ui/ContactsController.h"

#include "feature/ui/BadgeAggregator.h"
#include "feature/ui/SettingsController.h"
#include "base/crypto/PskFingerprint.h"
#include "base/i18n/LocalizationService.h"
#include "base/messaging/DirectChatTarget.h"
#include "base/messaging/MessagingJson.h"
#include "base/messaging/ThreadTypes.h"
#include "base/people/ContactTypes.h"
#include "base/people/Ed25519Signer.h"
#include "base/ui/ContextMenuHost.h"
#include "feature/ui/ChatSessionActions.h"
#include "feature/messaging/MessagingHub.h"
#include "feature/ui/DataModelHost.h"
#include "feature/ui/PinGateController.h"
#include "feature/ui/ShellFeedback.h"
#include "feature/ui/ShellHost.h"
#include "feature/ui/UiEditSession.h"
#include "feature/ui/UserFeedback.h"

#include "base/ui/ShellTypes.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/DataModelHandle.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/SystemInterface.h>

#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>
#include <sstream>

namespace pbr {

namespace {

constexpr uint32_t kContactDebounceMs = 400;

std::string TrimCopy(std::string text) {
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) {
    text.erase(text.begin());
  }
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) {
    text.pop_back();
  }
  return text;
}

std::vector<std::string> ParseMultiaddrsFromText(const std::string& text) {
  std::vector<std::string> out;
  std::istringstream stream(text);
  std::string line;
  while (std::getline(stream, line)) {
    line = TrimCopy(std::move(line));
    if (!line.empty()) {
      out.push_back(std::move(line));
    }
  }
  return out;
}

std::string MultiaddrsToText(const std::vector<std::string>& multiaddrs) {
  std::ostringstream out;
  for (size_t i = 0; i < multiaddrs.size(); ++i) {
    if (i > 0) {
      out << '\n';
    }
    out << multiaddrs[i];
  }
  return out.str();
}

bool IsContactDetailTransientActive(const ShellState& state) {
  return !state.transient_stack.empty() && state.transient_stack.back().spec.key == "contact_detail";
}

std::string IdentityKindLabel(const ContactIdKind kind) {
  switch (kind) {
  case ContactIdKind::RelayUser:
    return "Relay ID";
  case ContactIdKind::PeerId:
    return "Peer ID";
  case ContactIdKind::Blockchain:
    return "Blockchain";
  case ContactIdKind::Custom:
    return "Custom";
  }
  return "ID";
}

std::string TrustDisplayLabel(const TrustLevel level) {
  switch (level) {
  case TrustLevel::Friendly:
    return "Friendly";
  case TrustLevel::Blocked:
    return "Blocked";
  case TrustLevel::Unknown:
    return "Unknown";
  }
  return "Unknown";
}

std::string PrimaryIdOfKind(const Contact& contact, const ContactIdKind kind) {
  for (const ContactId& id : contact.ids) {
    if (id.kind == kind && id.primary) {
      return id.value;
    }
  }
  for (const ContactId& id : contact.ids) {
    if (id.kind == kind) {
      return id.value;
    }
  }
  return {};
}

std::string PrimaryIdentityValue(const Contact& contact) {
  if (const std::string relay = PrimaryIdOfKind(contact, ContactIdKind::RelayUser); !relay.empty()) {
    return relay;
  }
  if (const std::string peer = PrimaryIdOfKind(contact, ContactIdKind::PeerId); !peer.empty()) {
    return peer;
  }
  for (const ContactId& id : contact.ids) {
    if (id.primary) {
      return id.value;
    }
  }
  if (!contact.ids.empty()) {
    return contact.ids.front().value;
  }
  return {};
}

std::string ChannelLabel(const ThreadChannel channel) {
  switch (channel) {
  case ThreadChannel::E2e:
    return "Secure";
  case ThreadChannel::E2ePublic:
    return "Public";
  case ThreadChannel::None:
    return "Chat";
  }
  return "Chat";
}

std::string ThreadKindClass(const ThreadChannel channel) {
  switch (channel) {
  case ThreadChannel::E2e:
    return "private";
  case ThreadChannel::E2ePublic:
    return "public";
  case ThreadChannel::None:
    return "ai";
  }
  return "ai";
}

ContactsController::ContactListRow ToContactListRow(const Contact& contact) {
  ContactsController::ContactListRow row;
  row.id = contact.id.c_str();
  row.title = contact.display_name.empty() ? contact.server_nickname.c_str() : contact.display_name.c_str();
  if (row.title.empty()) {
    row.title = "New contact";
  }
  row.subtitle = PrimaryIdentityValue(contact).c_str();
  if (row.subtitle.empty() && !contact.server_nickname.empty() &&
      contact.server_nickname != contact.display_name) {
    row.subtitle = contact.server_nickname.c_str();
  }
  row.trust = TrustLevelToString(contact.trust).c_str();
  if (ContactsController::Instance().Hub().IsInitialized()) {
    row.unread_count = ContactsController::Instance().Hub().Inbox().SumUnreadForContact(contact.id);
    row.unread_display = FormatBadgeCount(row.unread_count).c_str();
  }
  return row;
}

ContactsController::ContactDetail ToContactDetail(const Contact& contact) {
  ContactsController::ContactDetail detail;
  detail.id = contact.id.c_str();
  detail.display_name = contact.display_name.c_str();
  detail.nickname = contact.server_nickname.c_str();
  detail.relay_id = PrimaryIdOfKind(contact, ContactIdKind::RelayUser).c_str();
  detail.peer_id = PrimaryIdOfKind(contact, ContactIdKind::PeerId).c_str();
  detail.trust = TrustDisplayLabel(contact.trust).c_str();
  detail.trust_key = TrustLevelToString(contact.trust).c_str();

  detail.identities.reserve(contact.ids.size());
  for (const ContactId& id : contact.ids) {
    ContactsController::ContactIdentityRow row;
    row.label = IdentityKindLabel(id.kind).c_str();
    row.value = id.value.c_str();
    row.kind = ContactIdKindToString(id.kind).c_str();
    row.is_primary = id.primary;
    detail.identities.push_back(std::move(row));
  }

  if (ContactsController::Instance().Hub().IsInitialized()) {
    const std::string relay_id = PrimaryIdOfKind(contact, ContactIdKind::RelayUser);
    if (!relay_id.empty()) {
      if (auto key = ContactsController::Instance().Hub().SigningKeys().Get(ContactIdKindToString(ContactIdKind::RelayUser),
                                                                relay_id)) {
        if (auto bytes = Ed25519Signer::FromBase64(key->signing_public_key_b64)) {
          if (auto digest = PskFingerprint::Compute(*bytes)) {
            detail.signing_fingerprint = PskFingerprint::FormatDisplay(*digest).c_str();
          }
        }
      }
    }

    if (auto threads = ContactsController::Instance().Hub().Inbox().ListThreads()) {
      for (const Thread& thread : *threads) {
        if (thread.kind != ThreadKind::Direct) {
          continue;
        }
        const bool matches = std::find(thread.participant_contact_ids.begin(), thread.participant_contact_ids.end(),
                                       contact.id) != thread.participant_contact_ids.end();
        if (!matches) {
          continue;
        }
        ContactsController::ContactThreadRow row;
        row.id = thread.id.c_str();
        row.title = thread.title.empty() ? ChannelLabel(thread.channel).c_str() : thread.title.c_str();
        row.channel_label = ChannelLabel(thread.channel).c_str();
        row.kind = ThreadKindClass(thread.channel).c_str();
        row.unread_count = thread.unread_count;
        row.unread_display = FormatBadgeCount(thread.unread_count).c_str();
        detail.threads.push_back(std::move(row));
      }
      std::sort(detail.threads.begin(), detail.threads.end(),
                [](const ContactsController::ContactThreadRow& a, const ContactsController::ContactThreadRow& b) {
                  return a.channel_label < b.channel_label;
                });
    }
  }
  return detail;
}

void ApplyMessagingEligibility(ContactsController::ContactDetail& detail, const Contact& contact) {
  detail.multiaddrs_text = MultiaddrsToText(contact.multiaddrs).c_str();
  const DirectChatTarget target = DirectChatTargetFromContact(contact, ThreadChannel::E2ePublic);
  if (target.peer_identity_value.empty()) {
    detail.can_message = false;
    detail.message_hint = "Add a relay ID, or a peer ID with multiaddr, to message.";
    return;
  }
  if (target.peer_identity_kind == ContactIdKindToString(ContactIdKind::PeerId) && contact.multiaddrs.empty()) {
    detail.can_message = false;
    detail.message_hint = "Add a multiaddr for direct messaging without relay.";
    return;
  }
  detail.can_message = true;
  if (contact.multiaddrs.empty()) {
    detail.message_hint = "Relay messaging available. Add a multiaddr for a direct link.";
  } else {
    detail.message_hint = "";
  }
}

Contact BuildContactFromDetail(const Contact& existing, const ContactsController::ContactDetail& detail) {
  Contact contact = existing;
  contact.display_name = detail.display_name.c_str();
  contact.server_nickname = detail.nickname.c_str();
  contact.ids.clear();
  for (const ContactId& id : existing.ids) {
    if (id.kind != ContactIdKind::RelayUser && id.kind != ContactIdKind::PeerId) {
      contact.ids.push_back(id);
    }
  }
  const std::string relay = detail.relay_id.c_str();
  const std::string peer = detail.peer_id.c_str();
  const bool has_relay = !relay.empty();
  if (has_relay) {
    contact.ids.push_back({ContactIdKind::RelayUser, relay, true});
  }
  if (!peer.empty()) {
    contact.ids.push_back({ContactIdKind::PeerId, peer, !has_relay});
  }
  contact.multiaddrs = ParseMultiaddrsFromText(detail.multiaddrs_text.c_str());
  return contact;
}

} // namespace

ContactsController::ContactsController() {
  redirectLogger("ContactsController");
}

ContactsController& ContactsController::Instance() {
  static ContactsController controller;
  return controller;
}
void ContactsController::BindMessaging(MessagingHub& messaging) {
  messaging_ = &messaging;
}

MessagingHub& ContactsController::Hub() {
  if (!messaging_) {
    throw std::runtime_error("ContactsController messaging not bound");
  }
  return *messaging_;
}

const MessagingHub& ContactsController::Hub() const {
  if (!messaging_) {
    throw std::runtime_error("ContactsController messaging not bound");
  }
  return *messaging_;
}


bool ContactsController::RegisterModel(Rml::Context* context) {
  if (!context) {
    return false;
  }
  context_ = context;

  const bool registered = DataModelHost::Instance().Register(context, "contacts", [](Rml::DataModelConstructor& ctor) {
    auto& controller = ContactsController::Instance();
    if (auto list_handle = ctor.RegisterStruct<ContactListRow>()) {
      list_handle.RegisterMember("id", &ContactListRow::id);
      list_handle.RegisterMember("title", &ContactListRow::title);
      list_handle.RegisterMember("subtitle", &ContactListRow::subtitle);
      list_handle.RegisterMember("trust", &ContactListRow::trust);
      list_handle.RegisterMember("unread_count", &ContactListRow::unread_count);
      list_handle.RegisterMember("unread_display", &ContactListRow::unread_display);
    }
    if (auto identity_handle = ctor.RegisterStruct<ContactIdentityRow>()) {
      identity_handle.RegisterMember("label", &ContactIdentityRow::label);
      identity_handle.RegisterMember("value", &ContactIdentityRow::value);
      identity_handle.RegisterMember("kind", &ContactIdentityRow::kind);
      identity_handle.RegisterMember("is_primary", &ContactIdentityRow::is_primary);
    }
    if (auto thread_handle = ctor.RegisterStruct<ContactThreadRow>()) {
      thread_handle.RegisterMember("id", &ContactThreadRow::id);
      thread_handle.RegisterMember("title", &ContactThreadRow::title);
      thread_handle.RegisterMember("channel_label", &ContactThreadRow::channel_label);
      thread_handle.RegisterMember("kind", &ContactThreadRow::kind);
      thread_handle.RegisterMember("unread_count", &ContactThreadRow::unread_count);
      thread_handle.RegisterMember("unread_display", &ContactThreadRow::unread_display);
    }
    if (auto detail_handle = ctor.RegisterStruct<ContactDetail>()) {
      detail_handle.RegisterMember("id", &ContactDetail::id);
      detail_handle.RegisterMember("display_name", &ContactDetail::display_name);
      detail_handle.RegisterMember("nickname", &ContactDetail::nickname);
      detail_handle.RegisterMember("relay_id", &ContactDetail::relay_id);
      detail_handle.RegisterMember("peer_id", &ContactDetail::peer_id);
      detail_handle.RegisterMember("multiaddrs_text", &ContactDetail::multiaddrs_text);
      detail_handle.RegisterMember("trust", &ContactDetail::trust);
      detail_handle.RegisterMember("trust_key", &ContactDetail::trust_key);
      detail_handle.RegisterMember("signing_fingerprint", &ContactDetail::signing_fingerprint);
      detail_handle.RegisterMember("message_hint", &ContactDetail::message_hint);
      detail_handle.RegisterMember("can_message", &ContactDetail::can_message);
      detail_handle.RegisterMember("identities", &ContactDetail::identities);
      detail_handle.RegisterMember("threads", &ContactDetail::threads);
    }
    ctor.RegisterArray<std::vector<ContactListRow>>();
    ctor.RegisterArray<std::vector<ContactIdentityRow>>();
    ctor.RegisterArray<std::vector<ContactThreadRow>>();
    ctor.Bind("contacts", &controller.contacts_);
    ctor.Bind("search_query", &controller.search_query_);
    ctor.Bind("compact_layout", &controller.compact_layout_);
    ctor.Bind("selected", &controller.selected_);
    ctor.BindEventCallback("select_contact", &ContactsController::SelectContactCallback);
    ctor.BindEventCallback("back_to_list", &ContactsController::BackToListCallback);
    ctor.BindEventCallback("start_chat", &ContactsController::StartChatCallback);
    ctor.BindEventCallback("secure_message", &ContactsController::SecureMessageCallback);
    ctor.BindEventCallback("on_add_contact_menu", &ContactsController::AddContactMenuCallback);
    ctor.BindEventCallback("find_someone", &ContactsController::FindSomeoneCallback);
    ctor.BindEventCallback("copy_contact_id", &ContactsController::CopyIdCallback);
    ctor.BindEventCallback("share_contact", &ContactsController::ShareContactCallback);
    ctor.BindEventCallback("set_trust", &ContactsController::SetTrustCallback);
    ctor.BindEventCallback("remove_contact", &ContactsController::RemoveContactCallback);
    ctor.BindEventCallback("open_thread", &ContactsController::OpenThreadCallback);
    ctor.BindEventCallback("on_search_changed", &ContactsController::OnSearchChangedCallback);
    ctor.BindEventCallback("on_contact_field_changed", &ContactsController::OnContactFieldChangedCallback);
  });

  if (registered) {
    ShellHost::Instance().SetOnTransientPopped([](const std::string& key) {
      if (key == "contact_detail") {
        ContactsController::Instance().OnDetailDismissed();
      }
      if (key == "settings_detail") {
        SettingsController::Instance().OnDetailDismissed();
      }
    });
  }

  return registered;
}

void ContactsController::DirtyAll() {
  auto& host = DataModelHost::Instance();
  host.Dirty("contacts", "contacts");
  host.Dirty("contacts", "search_query");
  host.Dirty("contacts", "compact_layout");
  host.Dirty("contacts", "selected");
}

void ContactsController::SyncLayoutMode() {
  const bool compact = ShellHost::Instance().State().layout_mode == LayoutMode::Compact;
  if (compact_layout_ == compact) {
    return;
  }

  compact_layout_ = compact;
  ShellState& state = ShellHost::Instance().State();
  if (!compact) {
    if (IsContactDetailTransientActive(state)) {
      ShellHost::Instance().PopTransient();
    }
    if (!selected_.id.empty()) {
      ShellHost::Instance().SetPrimaryPane("contact_detail");
    }
  } else if (!selected_.id.empty()) {
    ShellHost::Instance().ClearPrimaryPane();
    if (!IsContactDetailTransientActive(state)) {
      PaneSpec spec;
      spec.key = "contact_detail";
      ShellHost::Instance().PushTransient(spec);
    }
  } else if (IsContactDetailTransientActive(state)) {
    ShellHost::Instance().PopTransient();
  }
  DirtyAll();
}

void ContactsController::SyncFromStore() {
  contacts_.clear();
  if (!Instance().Hub().IsInitialized()) {
    return;
  }

  const std::string query = search_query_.c_str();
  auto stored = query.empty() ? Instance().Hub().Contacts().List()
                              : Instance().Hub().Contacts().SearchLocal(query);
  if (!stored) {
    return;
  }

  std::vector<Contact> sorted = *stored;
  std::sort(sorted.begin(), sorted.end(), [](const Contact& a, const Contact& b) {
    const std::string a_title = a.display_name.empty() ? a.server_nickname : a.display_name;
    const std::string b_title = b.display_name.empty() ? b.server_nickname : b.display_name;
    return a_title < b_title;
  });

  contacts_.reserve(sorted.size());
  for (const Contact& contact : sorted) {
    contacts_.push_back(ToContactListRow(contact));
  }
}

void ContactsController::Refresh() {
  FlushPending();
  SyncFromStore();
  if (!selected_.id.empty()) {
    LoadSelectedDetail(selected_.id.c_str());
  }
  DirtyAll();
  if (context_) {
    context_->Update();
  }
}

void ContactsController::OnNavTabActivated() {
  FlushPending();
  selected_ = {};
  contact_dirty_ = false;
  debounce_deadline_ms_ = 0;
  search_query_ = "";
  compact_layout_ = ShellHost::Instance().State().layout_mode == LayoutMode::Compact;
  SyncFromStore();
  DirtyAll();
  if (context_) {
    context_->Update();
  }
}

void ContactsController::SelectContactCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                               const Rml::VariantList& args) {
  if (args.empty() || args[0].GetType() != Rml::Variant::STRING) {
    return;
  }
  Instance().OnSelectContact(std::string(args[0].Get<Rml::String>().c_str()));
}

void ContactsController::BackToListCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                            const Rml::VariantList& /*args*/) {
  Instance().OnBackToList();
}

void ContactsController::StartChatCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                           const Rml::VariantList& /*args*/) {
  Instance().OnStartChat();
}

void ContactsController::SecureMessageCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                               const Rml::VariantList& /*args*/) {
  Instance().OnSecureMessage();
}

void ContactsController::AddContactMenuCallback(Rml::DataModelHandle /*model*/, Rml::Event& ev,
                                              const Rml::VariantList& /*args*/) {
  Instance().OnAddContactMenu(ev);
}

void ContactsController::FindSomeoneCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                             const Rml::VariantList& /*args*/) {
  Instance().OnFindSomeone();
}

void ContactsController::OnContactFieldChangedCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                                         const Rml::VariantList& /*args*/) {
  Instance().OnContactFieldChanged();
}

void ContactsController::CopyIdCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                        const Rml::VariantList& /*args*/) {
  Instance().OnCopyId();
}

void ContactsController::ShareContactCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                              const Rml::VariantList& /*args*/) {
  Instance().OnShareContact();
}

void ContactsController::SetTrustCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                          const Rml::VariantList& args) {
  if (args.empty() || args[0].GetType() != Rml::Variant::STRING) {
    return;
  }
  Instance().OnSetTrust(std::string(args[0].Get<Rml::String>().c_str()));
}

void ContactsController::RemoveContactCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                               const Rml::VariantList& /*args*/) {
  Instance().OnRemoveContact();
}

void ContactsController::OpenThreadCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                            const Rml::VariantList& args) {
  if (args.empty() || args[0].GetType() != Rml::Variant::STRING) {
    return;
  }
  Instance().OnOpenThread(std::string(args[0].Get<Rml::String>().c_str()));
}

void ContactsController::OnSearchChangedCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                                 const Rml::VariantList& /*args*/) {
  Instance().OnSearchChanged();
}

void ContactsController::LoadSelectedDetail(const std::string& contact_id) {
  if (!Instance().Hub().IsInitialized()) {
    selected_ = {};
    return;
  }
  auto contact = Instance().Hub().Contacts().Get(contact_id);
  if (!contact || !*contact) {
    selected_ = {};
    return;
  }
  selected_ = ToContactDetail(**contact);
  ApplyMessagingEligibility(selected_, **contact);
}

void ContactsController::UpdateMessagingEligibility(const Contact& contact) {
  ApplyMessagingEligibility(selected_, contact);
}

void ContactsController::OnSelectContact(const std::string& contact_id) {
  FlushPending();
  LoadSelectedDetail(contact_id);
  if (selected_.id.empty()) {
    return;
  }

  OpenContactDetailPane();
  DirtyAll();
}

void ContactsController::OpenContactDetailPane() {
  compact_layout_ = ShellHost::Instance().State().layout_mode == LayoutMode::Compact;
  ShellState& state = ShellHost::Instance().State();
  if (compact_layout_) {
    ShellHost::Instance().ClearPrimaryPane();
    if (!IsContactDetailTransientActive(state)) {
      PaneSpec spec;
      spec.key = "contact_detail";
      ShellHost::Instance().PushTransient(spec);
    }
    return;
  }

  if (IsContactDetailTransientActive(state)) {
    ShellHost::Instance().PopTransient();
  }
  ShellHost::Instance().SetPrimaryPane("contact_detail");
}

bool ContactsController::CloseContactDetailPane() {
  ShellState& state = ShellHost::Instance().State();
  if (IsContactDetailTransientActive(state)) {
    ShellHost::Instance().PopTransient();
    return true;
  }
  if (state.layout_mode != LayoutMode::Compact) {
    ShellHost::Instance().ClearPrimaryPane();
  }
  return false;
}

void ContactsController::OnDetailDismissed() {
  FlushPending();
  selected_ = {};
  contact_dirty_ = false;
  debounce_deadline_ms_ = 0;
  DirtyAll();
}

void ContactsController::OnBackToList() {
  FlushPending();
  if (!CloseContactDetailPane()) {
    OnDetailDismissed();
  }
}

void ContactsController::OnContactFieldChanged() {
  if (selected_.id.empty() || UiEditSession::Instance().RemountBlocking()) {
    return;
  }
  contact_dirty_ = true;
  debounce_deadline_ms_ = SDL_GetTicks() + kContactDebounceMs;

  if (Instance().Hub().IsInitialized()) {
    if (auto existing = Instance().Hub().Contacts().Get(selected_.id.c_str())) {
      if (*existing) {
        const Contact preview = BuildContactFromDetail(**existing, selected_);
        UpdateMessagingEligibility(preview);
        DataModelHost::Instance().Dirty("contacts", "selected");
      }
    }
  }
}

void ContactsController::Tick() {
  if (!contact_dirty_ || debounce_deadline_ms_ == 0) {
    return;
  }
  if (SDL_GetTicks() >= debounce_deadline_ms_) {
    debounce_deadline_ms_ = 0;
    (void)FlushSelectedContact();
  }
}

void ContactsController::FlushPending() {
  if (!contact_dirty_) {
    return;
  }
  debounce_deadline_ms_ = 0;
  (void)FlushSelectedContact();
}

bool ContactsController::FlushSelectedContact() {
  if (UiEditSession::Instance().RemountBlocking()) {
    return true; // keep dirty; Tick/FlushPending will retry after remount settles
  }
  if (!Instance().Hub().IsInitialized() || selected_.id.empty()) {
    contact_dirty_ = false;
    return true;
  }

  auto existing = Instance().Hub().Contacts().Get(selected_.id.c_str());
  if (!existing || !*existing) {
    contact_dirty_ = false;
    return false;
  }

  Contact updated = BuildContactFromDetail(**existing, selected_);
  if (!Instance().Hub().Contacts().Upsert(updated)) {
    UserFeedback::Fail("Could not save contact");
    ShellHost::Instance().DirtyWindow();
    return false;
  }

  Instance().Hub().P2p().RegisterContactDirectEndpoints(updated);
  UpdateMessagingEligibility(updated);
  contact_dirty_ = false;
  SyncFromStore();
  DataModelHost::Instance().Dirty("contacts", "contacts");
  DataModelHost::Instance().Dirty("contacts", "selected");
  Instance().Hub().Inbox().NotifyThreadChanged();
  if (context_) {
    context_->Update();
  }
  return true;
}

void ContactsController::OnAddContactMenu(Rml::Event& ev) {
  const Rml::Vector2i position = MenuPositionBelowEvent(ev);

  std::vector<ContextMenuAction> actions;
  actions.push_back({
      "add_contact",
      Tr("contacts.add"),
      nullptr,
      []() { ContactsController::Instance().OnAddContact(); },
      "../icons/contacts.svg",
  });
  actions.push_back({
      "find_someone",
      "Find someone",
      nullptr,
      []() { ContactsController::Instance().OnFindSomeone(); },
      "../icons/sparkle.svg",
  });
  ContextMenuHost::Instance().ShowActions(position, std::move(actions));
}

void ContactsController::OnAddContact() {
  if (!Instance().Hub().IsInitialized()) {
    return;
  }
  auto created = Instance().Hub().Contacts().AddEmpty();
  if (!created) {
    UserFeedback::Fail("Could not add contact");
    ShellHost::Instance().DirtyWindow();
    return;
  }
  SyncFromStore();
  OnSelectContact(created->id);
  DirtyAll();
  if (context_) {
    context_->Update();
  }
}

void ContactsController::OnStartChat() {
  if (!Instance().Hub().IsInitialized() || selected_.id.empty()) {
    return;
  }
  if (!selected_.can_message) {
    ShellFeedback::ShowToast(ShellHost::Instance().State(), selected_.message_hint.c_str());
    ShellHost::Instance().DirtyWindow();
    return;
  }
  FlushPending();

  const std::string contact_id = selected_.id.c_str();
  auto contact = Instance().Hub().Contacts().Get(contact_id);
  if (contact && *contact) {
    Instance().Hub().P2p().RegisterContactDirectEndpoints(**contact);
  }
  auto thread = Instance().Hub().Inbox().FindOrCreateDirectThread(contact_id, ThreadChannel::E2ePublic);
  if (!thread) {
    return;
  }
  Instance().Hub().P2p().WarmPeerForThread(thread->id);

  ShellHost::Instance().SelectNavTab(NavTab::Sessions);
  ShellHost::Instance().SetPrimaryPane("chat");
  ChatSessionActions::Instance().finalize_thread_display();
}

void ContactsController::OnSecureMessage() {
  if (!Instance().Hub().IsInitialized() || selected_.id.empty()) {
    return;
  }
  if (!selected_.can_message) {
    ShellFeedback::ShowToast(ShellHost::Instance().State(), selected_.message_hint.c_str());
    ShellHost::Instance().DirtyWindow();
    return;
  }
  FlushPending();

  PinGateController::Instance().EnsureUnlocked([this](const bool unlocked) {
    if (!unlocked) {
      ShellFeedback::ShowToast(ShellHost::Instance().State(), "PIN required to continue");
      ShellHost::Instance().DirtyWindow();
      return;
    }
    const std::string contact_id = selected_.id.c_str();
    auto contact = Instance().Hub().Contacts().Get(contact_id);
    if (contact && *contact) {
      Instance().Hub().P2p().RegisterContactDirectEndpoints(**contact);
    }
    auto thread = Instance().Hub().Inbox().FindOrCreateDirectThread(contact_id, ThreadChannel::E2e);
    if (!thread) {
      return;
    }
    (void)Instance().Hub().P2p().EnsurePskGenerated(thread->id);
    Instance().Hub().P2p().WarmPeerForThread(thread->id);

    ShellHost::Instance().SelectNavTab(NavTab::Sessions);
    ShellHost::Instance().SetPrimaryPane("chat");
    ChatSessionActions::Instance().finalize_thread_display();
  });
}

void ContactsController::OnFindSomeone() {
  ShellHost::Instance().SelectNavTab(NavTab::Sessions);
  if (ChatSessionActions::Instance().on_find_someone) {
    ChatSessionActions::Instance().on_find_someone();
  }
}

void ContactsController::OnCopyId() {
  std::string value = selected_.relay_id.c_str();
  const char* label = "Relay ID";
  if (value.empty()) {
    value = selected_.peer_id.c_str();
    label = "Peer ID";
  }
  if (value.empty() && !selected_.identities.empty()) {
    value = selected_.identities.front().value.c_str();
    label = "ID";
  }
  if (value.empty()) {
    ShellFeedback::ShowToast(ShellHost::Instance().State(), "No ID to copy");
    ShellHost::Instance().DirtyWindow();
    return;
  }
  if (Rml::SystemInterface* system = Rml::GetSystemInterface()) {
    system->SetClipboardText(value.c_str());
  }
  ShellFeedback::ShowToast(ShellHost::Instance().State(), std::string(label) + " copied");
  ShellHost::Instance().DirtyWindow();
}

void ContactsController::OnShareContact() {
  if (selected_.id.empty()) {
    return;
  }
  std::string invite = selected_.display_name.c_str();
  if (invite.empty()) {
    invite = selected_.nickname.c_str();
  }
  const std::string relay = selected_.relay_id.c_str();
  const std::string peer = selected_.peer_id.c_str();
  if (!peer.empty()) {
    if (!invite.empty()) {
      invite += " (" + peer + ")";
    } else {
      invite = peer;
    }
  }
  if (!relay.empty()) {
    if (!invite.empty()) {
      invite += " [" + relay + "]";
    } else {
      invite = relay;
    }
  }
  if (invite.empty()) {
    ShellFeedback::ShowToast(ShellHost::Instance().State(), "Nothing to share");
    ShellHost::Instance().DirtyWindow();
    return;
  }
  if (Rml::SystemInterface* system = Rml::GetSystemInterface()) {
    system->SetClipboardText(invite.c_str());
  }
  ShellFeedback::ShowToast(ShellHost::Instance().State(), "Contact copied");
  ShellHost::Instance().DirtyWindow();
}

void ContactsController::OnSetTrust(const std::string& trust) {
  if (!Instance().Hub().IsInitialized() || selected_.id.empty()) {
    return;
  }
  auto contact = Instance().Hub().Contacts().Get(selected_.id.c_str());
  if (!contact || !*contact) {
    return;
  }
  Contact updated = **contact;
  updated.trust = TrustLevelFromString(trust);
  if (!Instance().Hub().Contacts().Upsert(updated)) {
    UserFeedback::Fail("Could not update trust");
    ShellHost::Instance().DirtyWindow();
    return;
  }
  LoadSelectedDetail(selected_.id.c_str());
  SyncFromStore();
  DirtyAll();
  ShellFeedback::ShowToast(ShellHost::Instance().State(), "Trust updated");
  ShellHost::Instance().DirtyWindow();
}

void ContactsController::OnRemoveContact() {
  if (!Instance().Hub().IsInitialized() || selected_.id.empty()) {
    return;
  }

  const std::string contact_id = selected_.id.c_str();
  const std::string display_name = selected_.display_name.empty() ? "this contact" : selected_.display_name.c_str();
  const std::string message = "Remove " + display_name +
                              " from contacts? Conversations stay on this device. "
                              "You can add them again later from Find.";

  ShellFeedback::ShowConfirm(ShellHost::Instance().State(), Tr("contacts.remove"), message,
                             [this, contact_id](bool ok) {
                               if (!ok) {
                                 return;
                               }
                               auto removed = Instance().Hub().Contacts().Remove(contact_id);
                               if (!removed) {
                                 UserFeedback::Fail("Could not remove contact");
                                 ShellHost::Instance().DirtyWindow();
                                 return;
                               }
                               if (!*removed) {
                                 ShellFeedback::ShowToast(ShellHost::Instance().State(), "Contact not found");
                                 ShellHost::Instance().DirtyWindow();
                                 return;
                               }
                               if (!CloseContactDetailPane()) {
                                 OnDetailDismissed();
                               }
                               SyncFromStore();
                               DirtyAll();
                               ShellFeedback::ShowToast(ShellHost::Instance().State(), "Contact removed");
                               Instance().Hub().Inbox().NotifyThreadChanged();
                               ShellHost::Instance().RequestSyncLayout();
                               ShellHost::Instance().DirtyWindow();
                             });
}

void ContactsController::OnOpenThread(const std::string& thread_id) {
  if (thread_id.empty()) {
    return;
  }
  ShellHost::Instance().SelectNavTab(NavTab::Sessions);
  ShellHost::Instance().SetPrimaryPane("chat");
  if (ChatSessionActions::Instance().select_thread) {
    ChatSessionActions::Instance().select_thread(thread_id);
  }
}

void ContactsController::OnSearchChanged() {
  SyncFromStore();
  DataModelHost::Instance().Dirty("contacts", "contacts");
  DataModelHost::Instance().Dirty("contacts", "search_query");
}

} // namespace pbr
