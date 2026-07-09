#include "feature/ui/ContactsController.h"

#include "base/crypto/PskFingerprint.h"
#include "base/messaging/MessagingJson.h"
#include "base/messaging/ThreadTypes.h"
#include "base/people/ContactTypes.h"
#include "base/people/Ed25519Signer.h"
#include "feature/chat/ChatController.h"
#include "feature/messaging/MessagingHub.h"
#include "feature/ui/DataModelHost.h"
#include "feature/ui/ShellFeedback.h"
#include "feature/ui/ShellHost.h"

#include "base/ui/ShellTypes.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/DataModelHandle.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/SystemInterface.h>

#include <algorithm>

namespace pbr {

namespace {

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
  row.subtitle = PrimaryIdentityValue(contact).c_str();
  if (row.subtitle.empty() && !contact.server_nickname.empty() &&
      contact.server_nickname != contact.display_name) {
    row.subtitle = contact.server_nickname.c_str();
  }
  row.trust = TrustLevelToString(contact.trust).c_str();
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

  if (MessagingHub::Instance().IsInitialized()) {
    const std::string relay_id = PrimaryIdOfKind(contact, ContactIdKind::RelayUser);
    if (!relay_id.empty()) {
      if (auto key = MessagingHub::Instance().SigningKeys().Get(ContactIdKindToString(ContactIdKind::RelayUser),
                                                                relay_id)) {
        if (auto bytes = Ed25519Signer::FromBase64(key->signing_public_key_b64)) {
          if (auto digest = PskFingerprint::Compute(*bytes)) {
            detail.signing_fingerprint = PskFingerprint::FormatDisplay(*digest).c_str();
          }
        }
      }
    }

    if (auto threads = MessagingHub::Instance().Inbox().ListThreads()) {
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

} // namespace

ContactsController::ContactsController() {
  redirectLogger("ContactsController");
}

ContactsController& ContactsController::Instance() {
  static ContactsController controller;
  return controller;
}

bool ContactsController::RegisterModel(Rml::Context* context) {
  if (!context) {
    return false;
  }
  context_ = context;

  return DataModelHost::Instance().Register(context, "contacts", [](Rml::DataModelConstructor& ctor) {
    auto& controller = ContactsController::Instance();
    if (auto list_handle = ctor.RegisterStruct<ContactListRow>()) {
      list_handle.RegisterMember("id", &ContactListRow::id);
      list_handle.RegisterMember("title", &ContactListRow::title);
      list_handle.RegisterMember("subtitle", &ContactListRow::subtitle);
      list_handle.RegisterMember("trust", &ContactListRow::trust);
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
    }
    if (auto detail_handle = ctor.RegisterStruct<ContactDetail>()) {
      detail_handle.RegisterMember("id", &ContactDetail::id);
      detail_handle.RegisterMember("display_name", &ContactDetail::display_name);
      detail_handle.RegisterMember("nickname", &ContactDetail::nickname);
      detail_handle.RegisterMember("relay_id", &ContactDetail::relay_id);
      detail_handle.RegisterMember("peer_id", &ContactDetail::peer_id);
      detail_handle.RegisterMember("trust", &ContactDetail::trust);
      detail_handle.RegisterMember("trust_key", &ContactDetail::trust_key);
      detail_handle.RegisterMember("signing_fingerprint", &ContactDetail::signing_fingerprint);
      detail_handle.RegisterMember("identities", &ContactDetail::identities);
      detail_handle.RegisterMember("threads", &ContactDetail::threads);
    }
    ctor.RegisterArray<std::vector<ContactListRow>>();
    ctor.RegisterArray<std::vector<ContactIdentityRow>>();
    ctor.RegisterArray<std::vector<ContactThreadRow>>();
    ctor.Bind("contacts", &controller.contacts_);
    ctor.Bind("search_query", &controller.search_query_);
    ctor.Bind("compact_layout", &controller.compact_layout_);
    ctor.Bind("show_detail", &controller.show_detail_);
    ctor.Bind("selected", &controller.selected_);
    ctor.BindEventCallback("select_contact", &ContactsController::SelectContactCallback);
    ctor.BindEventCallback("back_to_list", &ContactsController::BackToListCallback);
    ctor.BindEventCallback("start_chat", &ContactsController::StartChatCallback);
    ctor.BindEventCallback("secure_message", &ContactsController::SecureMessageCallback);
    ctor.BindEventCallback("find_someone", &ContactsController::FindSomeoneCallback);
    ctor.BindEventCallback("copy_contact_id", &ContactsController::CopyIdCallback);
    ctor.BindEventCallback("share_contact", &ContactsController::ShareContactCallback);
    ctor.BindEventCallback("set_trust", &ContactsController::SetTrustCallback);
    ctor.BindEventCallback("open_thread", &ContactsController::OpenThreadCallback);
    ctor.BindEventCallback("on_search_changed", &ContactsController::OnSearchChangedCallback);
  });
}

void ContactsController::DirtyAll() {
  auto& host = DataModelHost::Instance();
  host.Dirty("contacts", "contacts");
  host.Dirty("contacts", "search_query");
  host.Dirty("contacts", "compact_layout");
  host.Dirty("contacts", "show_detail");
  host.Dirty("contacts", "selected");
}

void ContactsController::SyncLayoutMode() {
  const bool compact = ShellHost::Instance().State().layout_mode == LayoutMode::Compact;
  if (compact_layout_ == compact) {
    return;
  }

  compact_layout_ = compact;
  if (!compact) {
    show_detail_ = false;
    if (!selected_.id.empty()) {
      ShellHost::Instance().SetPrimaryPane("contact_detail");
    }
  } else if (!selected_.id.empty()) {
    show_detail_ = true;
    ShellHost::Instance().ClearPrimaryPane();
  }
  DirtyAll();
}

void ContactsController::SyncFromStore() {
  contacts_.clear();
  if (!MessagingHub::Instance().IsInitialized()) {
    return;
  }

  const std::string query = search_query_.c_str();
  auto stored = query.empty() ? MessagingHub::Instance().Contacts().List()
                              : MessagingHub::Instance().Contacts().SearchLocal(query);
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
  show_detail_ = false;
  selected_ = {};
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

void ContactsController::FindSomeoneCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                             const Rml::VariantList& /*args*/) {
  Instance().OnFindSomeone();
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
  if (!MessagingHub::Instance().IsInitialized()) {
    selected_ = {};
    return;
  }
  auto contact = MessagingHub::Instance().Contacts().Get(contact_id);
  if (!contact || !*contact) {
    selected_ = {};
    return;
  }
  selected_ = ToContactDetail(**contact);
}

void ContactsController::OnSelectContact(const std::string& contact_id) {
  LoadSelectedDetail(contact_id);
  if (selected_.id.empty()) {
    return;
  }

  compact_layout_ = ShellHost::Instance().State().layout_mode == LayoutMode::Compact;
  if (compact_layout_) {
    show_detail_ = true;
    ShellHost::Instance().ClearPrimaryPane();
  } else {
    show_detail_ = false;
    ShellHost::Instance().SetPrimaryPane("contact_detail");
  }
  DirtyAll();
}

void ContactsController::OnBackToList() {
  show_detail_ = false;
  selected_ = {};
  DirtyAll();
}

void ContactsController::OnStartChat() {
  if (!MessagingHub::Instance().IsInitialized() || selected_.id.empty()) {
    return;
  }

  const std::string contact_id = selected_.id.c_str();
  auto contact = MessagingHub::Instance().Contacts().Get(contact_id);
  if (contact && *contact) {
    MessagingHub::Instance().P2p().RegisterContactDirectEndpoints(**contact);
  }
  auto thread = MessagingHub::Instance().Inbox().FindOrCreateDirectThread(contact_id, ThreadChannel::E2ePublic);
  if (!thread) {
    return;
  }
  MessagingHub::Instance().P2p().WarmPeerForThread(thread->id);

  ShellHost::Instance().SelectNavTab(NavTab::Sessions);
  ShellHost::Instance().SetPrimaryPane("chat");
  ChatController::Instance().FinalizeThreadDisplay();
}

void ContactsController::OnSecureMessage() {
  if (!MessagingHub::Instance().IsInitialized() || selected_.id.empty()) {
    return;
  }

  const std::string contact_id = selected_.id.c_str();
  auto contact = MessagingHub::Instance().Contacts().Get(contact_id);
  if (contact && *contact) {
    MessagingHub::Instance().P2p().RegisterContactDirectEndpoints(**contact);
  }
  auto thread = MessagingHub::Instance().Inbox().FindOrCreateDirectThread(contact_id, ThreadChannel::E2e);
  if (!thread) {
    return;
  }
  (void)MessagingHub::Instance().P2p().EnsurePskGenerated(thread->id);
  MessagingHub::Instance().P2p().WarmPeerForThread(thread->id);

  ShellHost::Instance().SelectNavTab(NavTab::Sessions);
  ShellHost::Instance().SetPrimaryPane("chat");
  ChatController::Instance().FinalizeThreadDisplay();
}

void ContactsController::OnFindSomeone() {
  ShellHost::Instance().SelectNavTab(NavTab::Sessions);
  ChatController::Instance().OnFindSomeone();
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
  if (!MessagingHub::Instance().IsInitialized() || selected_.id.empty()) {
    return;
  }
  auto contact = MessagingHub::Instance().Contacts().Get(selected_.id.c_str());
  if (!contact || !*contact) {
    return;
  }
  Contact updated = **contact;
  updated.trust = TrustLevelFromString(trust);
  if (!MessagingHub::Instance().Contacts().Upsert(updated)) {
    ShellFeedback::ShowToast(ShellHost::Instance().State(), "Could not update trust");
    ShellHost::Instance().DirtyWindow();
    return;
  }
  LoadSelectedDetail(selected_.id.c_str());
  SyncFromStore();
  DirtyAll();
  ShellFeedback::ShowToast(ShellHost::Instance().State(), "Trust updated");
  ShellHost::Instance().DirtyWindow();
}

void ContactsController::OnOpenThread(const std::string& thread_id) {
  if (thread_id.empty()) {
    return;
  }
  ShellHost::Instance().SelectNavTab(NavTab::Sessions);
  ShellHost::Instance().SetPrimaryPane("chat");
  ChatController::Instance().OnSelectThread(thread_id);
}

void ContactsController::OnSearchChanged() {
  SyncFromStore();
  DataModelHost::Instance().Dirty("contacts", "contacts");
  DataModelHost::Instance().Dirty("contacts", "search_query");
}

} // namespace pbr
