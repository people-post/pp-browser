#include <stdexcept>
#include "feature/ui/ContactsController.h"

#include "feature/ui/BadgeAggregator.h"
#include "base/crypto/CryptoUtil.h"
#include "base/crypto/PskFingerprint.h"
#include "base/i18n/LocalizationService.h"
#include "base/messaging/DirectChatTarget.h"
#include "base/messaging/MessagingJson.h"
#include "base/messaging/ThreadTypes.h"
#include "base/people/ContactTypes.h"
#include "base/people/PeerDisplayLabel.h"
#include "base/ui/ContextMenuHost.h"
#include "common/Utilities.h"
#include "feature/ui/ChatSessionPorts.h"
#include "feature/messaging/ContactReachability.h"
#include "feature/messaging/MessagingContactsPorts.h"
#include "feature/ui/DataModelHost.h"
#include "feature/ui/UnlockEnsurePorts.h"
#include "feature/ui/UiEditSession.h"
#include "feature/ui/UserFeedback.h"

#include "base/ui/ShellTypes.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Core.h>
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

std::string MultiaddrsSummary(const std::vector<std::string>& multiaddrs) {
  if (multiaddrs.empty()) {
    return {};
  }
  if (multiaddrs.size() == 1) {
    return "1 address";
  }
  return std::to_string(multiaddrs.size()) + " addresses";
}

bool IsContactDetailTransientActive(const ShellChromeSnapshot& chrome) {
  return chrome.contact_detail_transient;
}

std::string IdentityKindLabel(const ContactIdKind kind) {
  switch (kind) {
  case ContactIdKind::Account:
    return "Account ID";
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
  if (const std::string account = PrimaryIdOfKind(contact, ContactIdKind::Account); !account.empty()) {
    return account;
  }
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

ContactsController::ContactListRow ToContactListRow(const Contact& contact, const MessagingContactsPorts& ports) {
  ContactsController::ContactListRow row;
  row.id = contact.id.c_str();
  const std::string title = ContactEffectiveTitle(contact);
  row.title = title.empty() ? "New contact" : title.c_str();
  row.subtitle = PrimaryIdentityValue(contact).c_str();
  if (row.subtitle.empty() && !contact.server_nickname.empty() &&
      contact.server_nickname != contact.display_name) {
    row.subtitle = contact.server_nickname.c_str();
  }
  row.trust = TrustLevelToString(contact.trust).c_str();
  if (ports.contact_icon_local_path) {
    const std::string path = ports.contact_icon_local_path(contact);
    if (!path.empty()) {
      row.has_icon = true;
      row.icon_src = path.c_str();
    }
  }
  if (ports.ensure_contact_icon_cached && contact.remote.icon && !contact.remote.icon->url.empty()) {
    ports.ensure_contact_icon_cached(contact);
  }
  if (ports.snapshot && ports.snapshot().initialized && ports.sum_unread_for_contact) {
    row.unread_count = ports.sum_unread_for_contact(contact.id);
    row.unread_display = FormatBadgeCount(row.unread_count).c_str();
  }
  return row;
}

ContactsController::ContactDetail ToContactDetail(const Contact& contact, const MessagingContactsPorts& ports) {
  ContactsController::ContactDetail detail;
  detail.id = contact.id.c_str();
  const std::string title = ContactEffectiveTitle(contact);
  detail.title = title.empty() ? "New contact" : title.c_str();
  detail.display_name = contact.local.display_name.c_str();
  detail.nickname = contact.remote.nickname.c_str();
  detail.relay_id = PrimaryIdOfKind(contact, ContactIdKind::RelayUser).c_str();
  detail.peer_id = PrimaryIdOfKind(contact, ContactIdKind::PeerId).c_str();
  detail.has_relay_id = !detail.relay_id.empty();
  if (!detail.relay_id.empty()) {
    detail.subtitle = ShortRelayId(detail.relay_id.c_str()).c_str();
  } else if (!detail.peer_id.empty()) {
    detail.subtitle = ShortRelayId(detail.peer_id.c_str()).c_str();
  }
  detail.trust = TrustDisplayLabel(contact.trust).c_str();
  detail.trust_key = TrustLevelToString(contact.trust).c_str();
  if (ports.contact_icon_local_path) {
    const std::string path = ports.contact_icon_local_path(contact);
    if (!path.empty()) {
      detail.has_icon = true;
      detail.icon_src = path.c_str();
    }
  }
  if (ports.ensure_contact_icon_cached && contact.remote.icon && !contact.remote.icon->url.empty()) {
    ports.ensure_contact_icon_cached(contact);
  }
  if (contact.remote.fetched_at > 0) {
    detail.remote_updated = "From directory";
  } else {
    detail.remote_updated = "Not synced yet";
  }

  detail.identities.reserve(contact.ids.size());
  for (const ContactId& id : contact.ids) {
    ContactsController::ContactIdentityRow row;
    row.label = IdentityKindLabel(id.kind).c_str();
    row.value = id.value.c_str();
    row.kind = ContactIdKindToString(id.kind).c_str();
    row.is_primary = id.primary;
    detail.identities.push_back(std::move(row));
  }

  if (ports.snapshot && ports.snapshot().initialized) {
    const std::string account_id = PrimaryIdOfKind(contact, ContactIdKind::Account);
    if (!account_id.empty() && ports.get_signing_key) {
      if (auto key = ports.get_signing_key(ContactIdKindToString(ContactIdKind::Account), account_id)) {
        if (auto bytes = Base64Decode(key->signing_public_key_b64)) {
          if (auto digest = PskFingerprint::Compute(*bytes)) {
            detail.signing_fingerprint = PskFingerprint::FormatDisplay(*digest).c_str();
          }
        }
      }
    }

    if (ports.list_threads) {
      if (auto threads = ports.list_threads()) {
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
  }
  return detail;
}

void ApplyMessagingEligibility(ContactsController::ContactDetail& detail, const Contact& contact,
                               const MessagingContactsPorts& ports) {
  detail.multiaddrs_text = MultiaddrsToText(contact.multiaddrs).c_str();
  detail.multiaddrs_summary = MultiaddrsSummary(contact.multiaddrs).c_str();
  const DirectChatTarget target = DirectChatTargetFromContact(contact, ThreadChannel::E2ePublic);
  if (target.peer_identity_value.empty()) {
    detail.can_message = false;
    detail.message_hint = "Add a relay ID, or a peer ID with multiaddr, to message.";
    return;
  }
  if (ports.snapshot && ports.snapshot().messaging_ready && ports.is_contact_reachable) {
    detail.can_message = ports.is_contact_reachable(contact);
    if (!detail.can_message) {
      detail.message_hint =
          "Add a relay ID or multiaddr, or connect on the same network so this PeerId becomes dialable.";
      return;
    }
    if (contact.multiaddrs.empty() && ports.sessions &&
        IsContactStackDialable(contact, ports.sessions())) {
      detail.message_hint = "Direct link via address book (no pasted multiaddr).";
    } else if (contact.multiaddrs.empty()) {
      detail.message_hint = "Relay messaging available. Add a multiaddr for a pinned direct link.";
    } else {
      detail.message_hint = "";
    }
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
  // Only local annotations are editable from the detail form.
  contact.local.display_name = detail.display_name.c_str();
  SyncContactMirrors(contact);
  return contact;
}

} // namespace

ContactsController* ContactsController::installed_instance_ = nullptr;

void ContactsController::InstallInstance(ContactsController& controller) {
  installed_instance_ = &controller;
}

void ContactsController::ClearInstance() {
  installed_instance_ = nullptr;
}

ContactsController& ContactsController::Instance() {
  if (!installed_instance_) {
    throw std::runtime_error("ContactsController not installed");
  }
  return *installed_instance_;
}

ContactsController::ContactsController() {
  redirectLogger("ContactsController");
}

void ContactsController::BindContactsPorts(MessagingContactsPorts ports) {
  contacts_ports_ = std::move(ports);
}

void ContactsController::BindUnlockEnsure(UnlockEnsurePorts ports) {
  unlock_ensure_ = std::move(ports);
}

void ContactsController::BindChatPorts(ChatSessionPorts ports) {
  chat_ports_ = std::move(ports);
}

void ContactsController::BindShellNavigation(ShellNavigationPorts ports) {
  shell_navigation_ = std::move(ports);
}

void ContactsController::BindShellFeedback(ShellFeedbackPorts ports) {
  shell_feedback_ = std::move(ports);
}

void ContactsController::BindSurfaceNotify(ContactsSurfaceNotifyPorts ports) {
  surface_notify_ = std::move(ports);
}

ShellChromeSnapshot ContactsController::ChromeSnapshot() const {
  return shell_navigation_.snapshot ? shell_navigation_.snapshot() : ShellChromeSnapshot{};
}

ContactsSurfaceSnapshot ContactsController::BuildSurfaceSnapshot() const {
  ContactsSurfaceSnapshot snap;
  snap.detail_open = !selected_.id.empty();
  for (const ContactListRow& row : contacts_) {
    snap.contacts_unread += row.unread_count;
  }
  return snap;
}

void ContactsController::NotifySurfaceChanged() {
  if (surface_notify_.push_surface) {
    surface_notify_.push_surface(BuildSurfaceSnapshot());
  }
}

void ContactsController::ShowToast(const std::string& message, const ToastDuration duration) {
  if (shell_feedback_.show_toast) {
    shell_feedback_.show_toast(message, duration);
  }
}

void ContactsController::ShowConfirm(const std::string& title, const std::string& message,
                                     std::function<void(bool)> on_result) {
  if (shell_feedback_.show_confirm) {
    shell_feedback_.show_confirm(title, message, std::move(on_result), {});
  }
}

void ContactsController::NavigateToChatSession() {
  if (shell_navigation_.select_nav_tab) {
    shell_navigation_.select_nav_tab(NavTab::Sessions);
  }
  if (shell_navigation_.set_primary_pane) {
    shell_navigation_.set_primary_pane("chat");
  }
  if (chat_ports_.finalize_thread_display) {
    chat_ports_.finalize_thread_display();
  }
}

bool ContactsController::RegisterModel(Rml::Context* context) {
  if (!context) {
    return false;
  }
  context_ = context;

  const bool registered = DataModelHost::Instance().Register(context, "contacts", [this](Rml::DataModelConstructor& ctor) {
    auto& controller = *this;
    if (auto list_handle = ctor.RegisterStruct<ContactListRow>()) {
      list_handle.RegisterMember("id", &ContactListRow::id);
      list_handle.RegisterMember("title", &ContactListRow::title);
      list_handle.RegisterMember("subtitle", &ContactListRow::subtitle);
      list_handle.RegisterMember("trust", &ContactListRow::trust);
      list_handle.RegisterMember("unread_count", &ContactListRow::unread_count);
      list_handle.RegisterMember("unread_display", &ContactListRow::unread_display);
      list_handle.RegisterMember("has_icon", &ContactListRow::has_icon);
      list_handle.RegisterMember("icon_src", &ContactListRow::icon_src);
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
    // Arrays must be registered before they are used as struct members.
    ctor.RegisterArray<std::vector<ContactListRow>>();
    ctor.RegisterArray<std::vector<ContactIdentityRow>>();
    ctor.RegisterArray<std::vector<ContactThreadRow>>();
    if (auto detail_handle = ctor.RegisterStruct<ContactDetail>()) {
      detail_handle.RegisterMember("id", &ContactDetail::id);
      detail_handle.RegisterMember("title", &ContactDetail::title);
      detail_handle.RegisterMember("subtitle", &ContactDetail::subtitle);
      detail_handle.RegisterMember("display_name", &ContactDetail::display_name);
      detail_handle.RegisterMember("nickname", &ContactDetail::nickname);
      detail_handle.RegisterMember("relay_id", &ContactDetail::relay_id);
      detail_handle.RegisterMember("peer_id", &ContactDetail::peer_id);
      detail_handle.RegisterMember("multiaddrs_text", &ContactDetail::multiaddrs_text);
      detail_handle.RegisterMember("multiaddrs_summary", &ContactDetail::multiaddrs_summary);
      detail_handle.RegisterMember("trust", &ContactDetail::trust);
      detail_handle.RegisterMember("trust_key", &ContactDetail::trust_key);
      detail_handle.RegisterMember("signing_fingerprint", &ContactDetail::signing_fingerprint);
      detail_handle.RegisterMember("message_hint", &ContactDetail::message_hint);
      detail_handle.RegisterMember("remote_updated", &ContactDetail::remote_updated);
      detail_handle.RegisterMember("can_message", &ContactDetail::can_message);
      detail_handle.RegisterMember("has_relay_id", &ContactDetail::has_relay_id);
      detail_handle.RegisterMember("has_icon", &ContactDetail::has_icon);
      detail_handle.RegisterMember("icon_src", &ContactDetail::icon_src);
      detail_handle.RegisterMember("identities", &ContactDetail::identities);
      detail_handle.RegisterMember("threads", &ContactDetail::threads);
    }
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
    ctor.BindEventCallback("sync_contact_remote", &ContactsController::SyncRemoteCallback);
  });

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
  const ShellChromeSnapshot chrome = ChromeSnapshot();
  const bool compact = chrome.layout_mode == LayoutMode::Compact;
  if (compact_layout_ == compact) {
    return;
  }

  compact_layout_ = compact;
  if (!compact) {
    if (IsContactDetailTransientActive(chrome) && shell_navigation_.pop_transient) {
      shell_navigation_.pop_transient();
    }
    if (!selected_.id.empty() && shell_navigation_.set_primary_pane) {
      shell_navigation_.set_primary_pane("contact_detail");
    }
  } else if (!selected_.id.empty()) {
    if (shell_navigation_.clear_primary_pane) {
      shell_navigation_.clear_primary_pane();
    }
    if (!IsContactDetailTransientActive(chrome) && shell_navigation_.push_transient) {
      PaneSpec spec;
      spec.key = "contact_detail";
      shell_navigation_.push_transient(spec);
    }
  } else if (IsContactDetailTransientActive(chrome) && shell_navigation_.pop_transient) {
    shell_navigation_.pop_transient();
  }
  DirtyAll();
}

void ContactsController::SyncFromStore() {
  contacts_.clear();
  if (!contacts_ports_.snapshot || !contacts_ports_.snapshot().initialized) {
    return;
  }

  const std::string query = search_query_.c_str();
  Roe<std::vector<Contact>> stored = query.empty()
                                          ? (contacts_ports_.list_contacts ? contacts_ports_.list_contacts()
                                                                           : Roe<std::vector<Contact>>::error(
                                                                                 Error("contacts port unavailable")))
                                          : (contacts_ports_.search_contacts
                                                 ? contacts_ports_.search_contacts(query)
                                                 : Roe<std::vector<Contact>>::error(Error("contacts port unavailable")));
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
    contacts_.push_back(ToContactListRow(contact, contacts_ports_));
  }
}

void ContactsController::Refresh() {
  FlushPending();
  SyncFromStore();
  if (!selected_.id.empty()) {
    LoadSelectedDetail(selected_.id.c_str());
  }
  DirtyAll();
  // Do not call context_->Update() here: Refresh runs from UI PostTasks (thread/message
  // notify, directory shadow reply) that can interleave with SyncLayout remounts.
  NotifySurfaceChanged();
}

void ContactsController::OnNavTabActivated() {
  FlushPending();
  selected_ = {};
  contact_dirty_ = false;
  debounce_deadline_ms_ = 0;
  search_query_ = "";
  compact_layout_ = ChromeSnapshot().layout_mode == LayoutMode::Compact;
  SyncFromStore();
  DirtyAll();
  NotifySurfaceChanged();
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

void ContactsController::SyncRemoteCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                            const Rml::VariantList& /*args*/) {
  Instance().OnSyncRemote();
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
  if (!contacts_ports_.snapshot || !contacts_ports_.snapshot().initialized || !contacts_ports_.get_contact) {
    selected_ = {};
    return;
  }
  auto contact = contacts_ports_.get_contact(contact_id);
  if (!contact || !*contact) {
    selected_ = {};
    return;
  }
  selected_ = ToContactDetail(**contact, contacts_ports_);
  ApplyMessagingEligibility(selected_, **contact, contacts_ports_);
}

void ContactsController::UpdateMessagingEligibility(const Contact& contact) {
  ApplyMessagingEligibility(selected_, contact, contacts_ports_);
}

void ContactsController::OnSelectContact(const std::string& contact_id) {
  FlushPending();
  LoadSelectedDetail(contact_id);
  if (selected_.id.empty()) {
    return;
  }

  OpenContactDetailPane();
  DirtyAll();
  NotifySurfaceChanged();
}

void ContactsController::OpenContactDetailPane() {
  const ShellChromeSnapshot chrome = ChromeSnapshot();
  compact_layout_ = chrome.layout_mode == LayoutMode::Compact;
  if (compact_layout_) {
    if (shell_navigation_.clear_primary_pane) {
      shell_navigation_.clear_primary_pane();
    }
    if (!IsContactDetailTransientActive(chrome) && shell_navigation_.push_transient) {
      PaneSpec spec;
      spec.key = "contact_detail";
      shell_navigation_.push_transient(spec);
    }
    return;
  }

  if (IsContactDetailTransientActive(chrome) && shell_navigation_.pop_transient) {
    shell_navigation_.pop_transient();
  }
  if (shell_navigation_.set_primary_pane) {
    shell_navigation_.set_primary_pane("contact_detail");
  }
}

bool ContactsController::CloseContactDetailPane() {
  const ShellChromeSnapshot chrome = ChromeSnapshot();
  if (IsContactDetailTransientActive(chrome) && shell_navigation_.pop_transient) {
    shell_navigation_.pop_transient();
    return true;
  }
  if (chrome.layout_mode != LayoutMode::Compact && shell_navigation_.clear_primary_pane) {
    shell_navigation_.clear_primary_pane();
  }
  return false;
}

void ContactsController::OnDetailDismissed() {
  FlushPending();
  selected_ = {};
  contact_dirty_ = false;
  debounce_deadline_ms_ = 0;
  DirtyAll();
  NotifySurfaceChanged();
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

  if (contacts_ports_.snapshot && contacts_ports_.snapshot().initialized && contacts_ports_.get_contact) {
    if (auto existing = contacts_ports_.get_contact(selected_.id.c_str())) {
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
  if (!contacts_ports_.snapshot || !contacts_ports_.snapshot().initialized || selected_.id.empty() ||
      !contacts_ports_.get_contact || !contacts_ports_.upsert_contact) {
    contact_dirty_ = false;
    return true;
  }

  auto existing = contacts_ports_.get_contact(selected_.id.c_str());
  if (!existing || !*existing) {
    contact_dirty_ = false;
    return false;
  }

  Contact updated = BuildContactFromDetail(**existing, selected_);
  if (!contacts_ports_.upsert_contact(updated)) {
    UserFeedback::Fail("Could not save contact");
    return false;
  }

  if (contacts_ports_.register_contact_direct_endpoints) {
    contacts_ports_.register_contact_direct_endpoints(updated);
  }
  UpdateMessagingEligibility(updated);
  contact_dirty_ = false;
  SyncFromStore();
  DataModelHost::Instance().Dirty("contacts", "contacts");
  DataModelHost::Instance().Dirty("contacts", "selected");
  if (contacts_ports_.notify_thread_changed) {
    contacts_ports_.notify_thread_changed();
  }
  NotifySurfaceChanged();
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
  if (!contacts_ports_.snapshot || !contacts_ports_.snapshot().initialized || !contacts_ports_.add_empty_contact) {
    return;
  }
  auto created = contacts_ports_.add_empty_contact();
  if (!created) {
    UserFeedback::Fail("Could not add contact");
    return;
  }
  SyncFromStore();
  OnSelectContact(created->id);
  DirtyAll();
  NotifySurfaceChanged();
}

void ContactsController::OnStartChat() {
  if (!contacts_ports_.snapshot || !contacts_ports_.snapshot().initialized || selected_.id.empty() ||
      !contacts_ports_.get_contact || !contacts_ports_.find_or_create_direct_thread) {
    return;
  }
  if (!selected_.can_message) {
    ShowToast(selected_.message_hint.c_str());
    return;
  }
  FlushPending();

  const std::string contact_id = selected_.id.c_str();
  auto contact = contacts_ports_.get_contact(contact_id);
  if (contact && *contact && contacts_ports_.register_contact_direct_endpoints) {
    contacts_ports_.register_contact_direct_endpoints(**contact);
  }
  auto thread = contacts_ports_.find_or_create_direct_thread(contact_id, ThreadChannel::E2ePublic);
  if (!thread) {
    return;
  }
  if (contacts_ports_.warm_peer_for_thread) {
    contacts_ports_.warm_peer_for_thread(thread->id);
  }
  NavigateToChatSession();
}

void ContactsController::OnSecureMessage() {
  if (!contacts_ports_.snapshot || !contacts_ports_.snapshot().initialized || selected_.id.empty() ||
      !contacts_ports_.get_contact || !contacts_ports_.find_or_create_direct_thread) {
    return;
  }
  if (!selected_.can_message) {
    ShowToast(selected_.message_hint.c_str());
    return;
  }
  FlushPending();

  if (!unlock_ensure_.ensure_unlocked) {
    return;
  }
  unlock_ensure_.ensure_unlocked([this](const bool unlocked) {
    if (!unlocked) {
      ShowToast("PIN required to continue");
      return;
    }
    const std::string contact_id = selected_.id.c_str();
    auto contact = contacts_ports_.get_contact(contact_id);
    if (contact && *contact && contacts_ports_.register_contact_direct_endpoints) {
      contacts_ports_.register_contact_direct_endpoints(**contact);
    }
    auto thread = contacts_ports_.find_or_create_direct_thread(contact_id, ThreadChannel::E2e);
    if (!thread) {
      return;
    }
    if (contacts_ports_.ensure_psk_generated) {
      (void)contacts_ports_.ensure_psk_generated(thread->id);
    }
    if (contacts_ports_.warm_peer_for_thread) {
      contacts_ports_.warm_peer_for_thread(thread->id);
    }
    NavigateToChatSession();
  });
}

void ContactsController::OnFindSomeone() {
  if (shell_navigation_.select_nav_tab) {
    shell_navigation_.select_nav_tab(NavTab::Sessions);
  }
  if (chat_ports_.on_find_someone) {
    chat_ports_.on_find_someone();
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
    ShowToast("No ID to copy");
    return;
  }
  if (Rml::SystemInterface* system = Rml::GetSystemInterface()) {
    system->SetClipboardText(value.c_str());
  }
  ShowToast(std::string(label) + " copied");
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
    ShowToast("Nothing to share");
    return;
  }
  if (Rml::SystemInterface* system = Rml::GetSystemInterface()) {
    system->SetClipboardText(invite.c_str());
  }
  ShowToast("Contact copied");
}

void ContactsController::OnSetTrust(const std::string& trust) {
  if (!contacts_ports_.snapshot || !contacts_ports_.snapshot().initialized || selected_.id.empty() ||
      !contacts_ports_.get_contact || !contacts_ports_.upsert_contact) {
    return;
  }
  auto contact = contacts_ports_.get_contact(selected_.id.c_str());
  if (!contact || !*contact) {
    return;
  }
  Contact updated = **contact;
  updated.local.trust = TrustLevelFromString(trust);
  SyncContactMirrors(updated);
  if (!contacts_ports_.upsert_contact(updated)) {
    UserFeedback::Fail("Could not update trust");
    return;
  }
  LoadSelectedDetail(selected_.id.c_str());
  SyncFromStore();
  DirtyAll();
  ShowToast("Trust updated");
  NotifySurfaceChanged();
}

void ContactsController::OnSyncRemote() {
  if (!contacts_ports_.snapshot || !contacts_ports_.snapshot().initialized || selected_.id.empty() ||
      !contacts_ports_.lookup_relay_user || !contacts_ports_.apply_remote_snapshot) {
    return;
  }
  FlushPending();
  const std::string relay_id = selected_.relay_id.c_str();
  if (relay_id.empty()) {
    UserFeedback::Fail("No relay ID to sync");
    return;
  }
  auto hit = contacts_ports_.lookup_relay_user(relay_id);
  if (!hit) {
    UserFeedback::Fail(hit.error().message.empty() ? "Could not sync contact" : hit.error().message);
    return;
  }
  if (hit->signing_public_key_b64 && !hit->signing_public_key_b64->empty() && contacts_ports_.register_peer_signing_key) {
    if (!hit->account_id || hit->account_id->empty()) {
      UserFeedback::Fail("Directory hit missing Account ID");
      return;
    }
    contacts_ports_.register_peer_signing_key(ContactIdKindToString(ContactIdKind::Account), *hit->account_id,
                                              *hit->signing_public_key_b64, "directory");
  }
  if (hit->kem_public_key_b64 && !hit->kem_public_key_b64->empty() && contacts_ports_.register_peer_kem_key) {
    if (!hit->account_id || hit->account_id->empty()) {
      UserFeedback::Fail("Directory hit missing Account ID");
      return;
    }
    contacts_ports_.register_peer_kem_key(ContactIdKindToString(ContactIdKind::Account), *hit->account_id,
                                          *hit->kem_public_key_b64, "directory");
  }
  auto applied = contacts_ports_.apply_remote_snapshot(selected_.id.c_str(), *hit, util::NowUnixMs());
  if (!applied) {
    UserFeedback::Fail(applied.error().message.empty() ? "Could not save synced contact"
                                                       : applied.error().message);
    return;
  }
  if (contacts_ports_.register_contact_direct_endpoints) {
    contacts_ports_.register_contact_direct_endpoints(*applied);
  }
  LoadSelectedDetail(selected_.id.c_str());
  SyncFromStore();
  DirtyAll();
  ShowToast("Contact synced");
  NotifySurfaceChanged();
}

void ContactsController::OnRemoveContact() {
  if (!contacts_ports_.snapshot || !contacts_ports_.snapshot().initialized || selected_.id.empty() ||
      !contacts_ports_.remove_contact) {
    return;
  }

  const std::string contact_id = selected_.id.c_str();
  const std::string display_name = selected_.display_name.empty() ? "this contact" : selected_.display_name.c_str();
  const std::string message = "Remove " + display_name +
                              " from contacts? Conversations stay on this device. "
                              "You can add them again later from Find.";

  ShowConfirm(Tr("contacts.remove"), message, [this, contact_id](const bool ok) {
    if (!ok) {
      return;
    }
    auto removed = contacts_ports_.remove_contact(contact_id);
    if (!removed) {
      UserFeedback::Fail("Could not remove contact");
      return;
    }
    if (!*removed) {
      ShowToast("Contact not found");
      return;
    }
    if (!CloseContactDetailPane()) {
      OnDetailDismissed();
    }
    SyncFromStore();
    DirtyAll();
    ShowToast("Contact removed");
    if (contacts_ports_.notify_thread_changed) {
      contacts_ports_.notify_thread_changed();
    }
    NotifySurfaceChanged();
  });
}

void ContactsController::OnOpenThread(const std::string& thread_id) {
  if (thread_id.empty()) {
    return;
  }
  if (shell_navigation_.select_nav_tab) {
    shell_navigation_.select_nav_tab(NavTab::Sessions);
  }
  if (shell_navigation_.set_primary_pane) {
    shell_navigation_.set_primary_pane("chat");
  }
  if (chat_ports_.select_thread) {
    chat_ports_.select_thread(thread_id);
  }
}

void ContactsController::OnSearchChanged() {
  SyncFromStore();
  DataModelHost::Instance().Dirty("contacts", "contacts");
  DataModelHost::Instance().Dirty("contacts", "search_query");
}

} // namespace pbr
