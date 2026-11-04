#include <stdexcept>
#include "gui/contacts/ContactsController.h"
#include "foundation/data/SessionStore.h"
#include "foundation/platform/DeploymentProfile.h"
#include "foundation/product/DevTools.h"
#include "common/ContactLimits.h"

#include "lib/messaging/ContactLocator.h"

#include "gui/BadgeAggregator.h"
#include "foundation/crypto/CryptoUtil.h"
#include "lib/delivery/PskFingerprint.h"
#include "foundation/i18n/LocalizationService.h"
#include "lib/delivery/DirectChatTargetFromContact.h"
#include "common/chat/MessagingJson.h"
#include "common/directory/DirectoryJson.h"
#include "common/directory/ShortRelayId.h"
#include "foundation/platform/ui/RmlUi_Backend.h"
#include "gui/AccountInvite.h"
#include "lib/ui/QrSvg.h"
#include "common/thread/ThreadTypes.h"
#include "lib/people/AvatarGlyph.h"
#include "lib/people/ContactJson.h"
#include "lib/people/ContactTypes.h"
#include "common/runtime/AppRuntime.h"
#include "lib/people/ContactIdentity.h"
#include "lib/people/DirectoryHitRank.h"
#include "lib/people/PeopleLookupFailure.h"
#include "gui/contacts/AddFromSearchDecision.h"
#include "foundation/error/AppError.h"
#include "lib/people/PeerDisplayLabel.h"
#include "lib/ui/ContextMenuHost.h"
#include "lib/ui/DataEvent.h"
#include "common/Utilities.h"
#include "common/Metrics.h"
#include "gui/ChatSessionPorts.h"
#include "lib/people/ContactReachability.h"
#include "feature/conversations/MessagingContactsPorts.h"
#include "gui/shell/DataModelHost.h"
#include "gui/UnlockEnsurePorts.h"
#include "lib/ui/UiEditSession.h"
#include "lib/ui/NameIndex.h"
#include "lib/ui/NewChatList.h"
#include "gui/UserFeedback.h"

#include "lib/ui/ShellTypes.h"

#include <ui/dom/Context.h>
#include <ui/dom/Element.h>
#include <ui/Core.h>
#include <ui/data/DataModelHandle.h>
#include <ui/dom/Event.h>
#include <ui/base/SystemInterface.h>

#include <SDL3/SDL.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <thread>
#include <string_view>
#include <utility>
#include <cctype>
#include <sstream>
#include "common/PbrCompat.h"
#include "common/MeshAlias.h"

namespace pbr {

namespace {

constexpr uint32_t kContactDebounceMs = 400;
/** Typing paused this long (and saved): the edit is over, the "Note saved" pill shows (once, RemarkSaveNotice). */
constexpr uint32_t kRemarkNoticeQuietMs = 1500;

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
    return Tr("contacts.addresses_one");
  }
  return Tr("contacts.addresses_many", {{"count", std::to_string(multiaddrs.size())}});
}

bool IsContactDetailTransientActive(const ShellChromeSnapshot& chrome) {
  return chrome.contact_detail_transient;
}

std::string IdentityKindLabel(const mesh::ContactIdKind kind) {
  switch (kind) {
  case mesh::ContactIdKind::Account:
    return Tr("contacts.id_kind.account");
  case mesh::ContactIdKind::RelayUser:
    return Tr("contacts.id_kind.relay");
  case mesh::ContactIdKind::PeerId:
    return Tr("contacts.id_kind.peer");
  case mesh::ContactIdKind::Blockchain:
    return Tr("contacts.id_kind.blockchain");
  case mesh::ContactIdKind::Custom:
    return Tr("contacts.id_kind.custom");
  }
  return Tr("contacts.id_kind.generic");
}

std::string TrustDisplayLabel(const TrustLevel level) {
  switch (level) {
  case TrustLevel::Friendly:
    return Tr("contacts.trust.friendly");
  case TrustLevel::Blocked:
    return Tr("contacts.trust.blocked");
  case TrustLevel::Unknown:
    return Tr("contacts.trust.unknown");
  }
  return Tr("contacts.trust.unknown");
}

std::string PrimaryIdentityValue(const Contact& contact) {
  if (const std::string account = PrimaryIdOfKind(contact, mesh::ContactIdKind::Account); !account.empty()) {
    return account;
  }
  if (const std::string relay = PrimaryIdOfKind(contact, mesh::ContactIdKind::RelayUser); !relay.empty()) {
    return relay;
  }
  if (const std::string peer = PrimaryIdOfKind(contact, mesh::ContactIdKind::PeerId); !peer.empty()) {
    return peer;
  }
  for (const mesh::ContactId& id : contact.ids) {
    if (id.primary) {
      return id.value;
    }
  }
  if (!contact.ids.empty()) {
    return contact.ids.front().value;
  }
  return {};
}

/** The last 6 characters of an id: enough to tell people apart without showing the whole id. */
std::string LastSixChars(const std::string& id) {
  constexpr size_t kKeep = 6;
  return id.size() <= kKeep ? id : id.substr(id.size() - kKeep);
}

std::string ChannelLabel(const ThreadChannel channel) {
  switch (channel) {
  case ThreadChannel::E2e:
    return Tr("contacts.channel.secure");
  case ThreadChannel::E2ePublic:
    return Tr("contacts.channel.public");
  case ThreadChannel::None:
    return Tr("contacts.channel.chat");
  }
  return Tr("contacts.channel.chat");
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
  row.title = title.empty() ? Tr("people_picker.unnamed").c_str() : title.c_str();
  row.subtitle = PrimaryIdentityValue(contact).c_str();
  if (row.subtitle.empty() && !contact.server_nickname.empty() &&
      contact.server_nickname != contact.display_name) {
    row.subtitle = contact.server_nickname.c_str();
  }
  {
    const std::string primary = PrimaryIdentityValue(contact);
    row.short_id = (primary.empty() ? row.subtitle.c_str() : LastSixChars(primary).c_str());
  }
  row.trust = TrustLevelToString(contact.trust).c_str();
  if (ports.contact_icon_local_path) {
    const std::string path = ports.contact_icon_local_path(contact);
    if (!path.empty()) {
      row.has_icon = true;
      row.icon_src = path.c_str();
    }
  }
  {
    // Colour by display name only, as the Chats list does: one person, one colour everywhere.
    const AvatarGlyph glyph = MakeAvatarGlyph(title.empty() ? Tr("people_picker.unnamed") : title, {});
    row.avatar_letter = glyph.letter.c_str();
    row.avatar_tone = glyph.tone;
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

} // namespace

bool ContactMatchesQuery(const Contact& contact, const std::string& query) {
  if (query.empty()) {
    return true;
  }
  if (TextMatchesQuery(ContactEffectiveTitle(contact), query) || TextMatchesQuery(contact.display_name, query) ||
      TextMatchesQuery(contact.server_nickname, query)) {
    return true;
  }
  // Account IDs match as plain text (a fragment of the ID), not by initials.
  const auto lower = [](std::string text) {
    for (char& c : text) {
      c = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    }
    return text;
  };
  const std::string needle = lower(query);
  for (const mesh::ContactId& id : contact.ids) {
    if (lower(id.value).find(needle) != std::string::npos) {
      return true;
    }
  }
  return false;
}

ContactsController::ContactDetail ToContactDetail(const Contact& contact, const MessagingContactsPorts& ports) {
  ContactsController::ContactDetail detail;
  detail.id = contact.id.c_str();
  const std::string title = ContactEffectiveTitle(contact);
  detail.title = title.empty() ? Tr("people_picker.unnamed").c_str() : title.c_str();
  detail.display_name = contact.local.display_name.c_str();
  detail.nickname = contact.remote.nickname.c_str();
  detail.name_placeholder = Tr("contacts.remark.placeholder").c_str();
  if (!contact.remote.nickname.empty()) {
    detail.their_nickname = Tr("contacts.remark.their_nickname", {{"nickname", contact.remote.nickname}}).c_str();
  }
  detail.relay_id = PrimaryIdOfKind(contact, mesh::ContactIdKind::RelayUser).c_str();
  detail.peer_id = PrimaryIdOfKind(contact, mesh::ContactIdKind::PeerId).c_str();
  detail.has_relay_id = !detail.relay_id.empty();
  if (!detail.relay_id.empty()) {
    detail.subtitle = ShortRelayId(detail.relay_id.c_str()).c_str();
  } else if (!detail.peer_id.empty()) {
    detail.subtitle = ShortRelayId(detail.peer_id.c_str()).c_str();
  }
  {
    const std::string primary = PrimaryIdentityValue(contact);
    detail.short_id = primary.empty() ? detail.subtitle : ui::String(LastSixChars(primary).c_str());
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
  {
    // Colour by display name only, as the Chats list does: one person, one colour everywhere.
    const AvatarGlyph glyph = MakeAvatarGlyph(title.empty() ? Tr("people_picker.unnamed") : title, {});
    detail.avatar_letter = glyph.letter.c_str();
    detail.avatar_tone = glyph.tone;
  }
  if (ports.ensure_contact_icon_cached && contact.remote.icon && !contact.remote.icon->url.empty()) {
    ports.ensure_contact_icon_cached(contact);
  }
  if (contact.remote.fetched_at > 0) {
    detail.remote_updated = Tr("contacts.from_directory").c_str();
  } else {
    detail.remote_updated = Tr("contacts.not_synced").c_str();
  }

  detail.identities.reserve(contact.ids.size());
  for (const mesh::ContactId& id : contact.ids) {
    ContactsController::ContactIdentityRow row;
    row.label = IdentityKindLabel(id.kind).c_str();
    row.value = id.value.c_str();
    row.kind = ContactIdKindToString(id.kind).c_str();
    row.is_primary = id.primary;
    detail.identities.push_back(std::move(row));
  }

  if (ports.snapshot && ports.snapshot().initialized) {
    const std::string account_id = PrimaryIdOfKind(contact, mesh::ContactIdKind::Account);
    if (!account_id.empty() && ports.get_signing_key) {
      if (auto key = ports.get_signing_key(ContactIdKindToString(mesh::ContactIdKind::Account), account_id)) {
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

namespace {

void ApplyMessagingEligibility(ContactsController::ContactDetail& detail, const Contact& contact,
                               const MessagingContactsPorts& ports) {
  detail.multiaddrs_text = MultiaddrsToText(contact.multiaddrs).c_str();
  detail.multiaddrs_summary = MultiaddrsSummary(contact.multiaddrs).c_str();
  const DirectChatTarget target = DirectChatTargetFromContact(contact, ThreadChannel::E2ePublic);
  if (target.peer_identity_value.empty()) {
    detail.can_message = false;
    detail.message_hint = Tr("contacts.hint.add_id").c_str();
    return;
  }
  if (ports.snapshot && ports.snapshot().messaging_ready && ports.is_contact_reachable) {
    detail.can_message = ports.is_contact_reachable(contact);
    if (!detail.can_message) {
      // The offline demo app reaches nobody by design; the hint would sit on every contact.
      if (PeerLinkStatusVisible(kUiDemoBuild)) {
        detail.message_hint = Tr("contacts.hint.not_dialable").c_str();
      }
      return;
    }
    if (contact.multiaddrs.empty()) {
      detail.message_hint = Tr("contacts.hint.relay_pinned").c_str();
    } else {
      detail.message_hint = "";
    }
    return;
  }
  if (target.peer_identity_kind == ContactIdKindToString(mesh::ContactIdKind::PeerId) && contact.multiaddrs.empty()) {
    detail.can_message = false;
    detail.message_hint = Tr("contacts.hint.direct_no_relay").c_str();
    return;
  }
  detail.can_message = true;
  if (contact.multiaddrs.empty()) {
    detail.message_hint = Tr("contacts.hint.relay_direct").c_str();
  } else {
    detail.message_hint = "";
  }
}

} // namespace

ContactsController::ContactDetail MemberPreviewDetail(const std::string& account_id, const std::string& name,
                                                      const std::string& icon_src) {
  ContactsController::ContactDetail detail;
  const std::string title = name.empty() ? Tr("people_picker.unnamed") : name;
  detail.title = title.c_str();
  detail.short_id = LastSixChars(account_id).c_str();
  detail.has_icon = !icon_src.empty();
  detail.icon_src = icon_src.c_str();
  // Same colour input as everywhere else (the name only), so the avatar matches the one in the chat.
  const AvatarGlyph glyph = MakeAvatarGlyph(title, {});
  detail.avatar_letter = glyph.letter.c_str();
  detail.avatar_tone = glyph.tone;
  detail.member_preview = true;
  detail.member_account = account_id.c_str();
  return detail;
}

Contact BuildContactFromDetail(const Contact& existing, const ContactsController::ContactDetail& detail) {
  Contact contact = existing;
  // Only local annotations are editable from the detail form.
  contact.local.display_name = TruncateCodePoints(detail.display_name.c_str(), kMaxNicknameChars);
  SyncContactMirrors(contact);
  return contact;
}

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

void ContactsController::BindFlowCoordinator(FlowCoordinatorPorts ports) {
  flow_coordinator_ = std::move(ports);
}

void ContactsController::BindShellFeedback(ShellFeedbackPorts ports) {
  shell_feedback_ = std::move(ports);
}

void ContactsController::BindSurfaceNotify(ContactsSurfaceNotifyPorts ports) {
  surface_notify_ = std::move(ports);
}

void ContactsController::BindScanPorts(ScanPorts ports) {
  scan_ports_ = std::move(ports);
}

void ContactsController::BindAccountPorts(ContactsAccountPorts ports) {
  account_ports_ = std::move(ports);
}

void ContactsController::BindSessionStore(SessionStore* store) {
  session_store_ = store;
}

std::vector<std::string> ContactsController::Starred() const {
  return session_store_ ? session_store_->Snapshot().profile_prefs.starred_contacts : std::vector<std::string>{};
}

ShellChromeSnapshot ContactsController::ChromeSnapshot() const {
  return shell_navigation_.snapshot ? shell_navigation_.snapshot() : ShellChromeSnapshot{};
}

ContactsSurfaceSnapshot ContactsController::BuildSurfaceSnapshot() const {
  ContactsSurfaceSnapshot snap;
  snap.detail_open = !selected_.id.empty();
  // The Contacts tab badge is reserved for the tab's own queues (docs/ui/WINDOW_SHELL.md); chat unread is
  // the Sessions tab's badge. Summing the rows' unread here lit both tabs for every message (Kenneth,
  // 2026-10-07). The per-contact count stays on the rows.
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
                                     std::function<void(bool)> on_result, const bool destructive) {
  if (destructive && shell_feedback_.show_confirm_destructive) {
    shell_feedback_.show_confirm_destructive(title, message, std::move(on_result), {});
  } else if (shell_feedback_.show_confirm) {
    shell_feedback_.show_confirm(title, message, std::move(on_result), {});
  }
}

void ContactsController::NavigateToChatSession() {
  CloseSheetsBeforeLeaving();
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

bool ContactsController::RegisterModel(ui::Context* context) {
  if (!context) {
    return false;
  }
  context_ = context;

  const bool registered = DataModelHost::Instance().Register(context, "contacts", [this](ui::DataModelConstructor& ctor) {
    auto& controller = *this;
    if (auto list_handle = ctor.RegisterStruct<ContactListRow>()) {
      list_handle.RegisterMember("id", &ContactListRow::id);
      list_handle.RegisterMember("title", &ContactListRow::title);
      list_handle.RegisterMember("subtitle", &ContactListRow::subtitle);
      list_handle.RegisterMember("short_id", &ContactListRow::short_id);
      list_handle.RegisterMember("trust", &ContactListRow::trust);
      list_handle.RegisterMember("unread_count", &ContactListRow::unread_count);
      list_handle.RegisterMember("unread_display", &ContactListRow::unread_display);
      list_handle.RegisterMember("has_icon", &ContactListRow::has_icon);
      list_handle.RegisterMember("icon_src", &ContactListRow::icon_src);
      list_handle.RegisterMember("avatar_letter", &ContactListRow::avatar_letter);
      list_handle.RegisterMember("avatar_tone", &ContactListRow::avatar_tone);
      list_handle.RegisterMember("starred", &ContactListRow::starred);
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
    if (auto hit_handle = ctor.RegisterStruct<AddHitRow>()) {
      hit_handle.RegisterMember("index", &AddHitRow::index);
      hit_handle.RegisterMember("title", &AddHitRow::title);
      hit_handle.RegisterMember("short_id", &AddHitRow::short_id);
      hit_handle.RegisterMember("avatar_letter", &AddHitRow::avatar_letter);
      hit_handle.RegisterMember("avatar_tone", &AddHitRow::avatar_tone);
    }
    // Arrays must be registered before they are used as struct members.
    ctor.RegisterArray<std::vector<AddHitRow>>();
    ctor.RegisterArray<std::vector<ContactListRow>>();
    if (auto section_handle = ctor.RegisterStruct<NewChatSectionRow>()) {
      section_handle.RegisterMember("letter", &NewChatSectionRow::letter);
      section_handle.RegisterMember("rows", &NewChatSectionRow::rows);
    }
    ctor.RegisterArray<std::vector<NewChatSectionRow>>();
    if (auto index_handle = ctor.RegisterStruct<IndexLetterRow>()) {
      index_handle.RegisterMember("letter", &IndexLetterRow::letter);
      index_handle.RegisterMember("present", &IndexLetterRow::present);
    }
    ctor.RegisterArray<std::vector<IndexLetterRow>>();
    ctor.RegisterArray<std::vector<ContactIdentityRow>>();
    ctor.RegisterArray<std::vector<ContactThreadRow>>();
    if (auto detail_handle = ctor.RegisterStruct<ContactDetail>()) {
      detail_handle.RegisterMember("id", &ContactDetail::id);
      detail_handle.RegisterMember("title", &ContactDetail::title);
      detail_handle.RegisterMember("subtitle", &ContactDetail::subtitle);
      detail_handle.RegisterMember("short_id", &ContactDetail::short_id);
      detail_handle.RegisterMember("display_name", &ContactDetail::display_name);
      detail_handle.RegisterMember("nickname", &ContactDetail::nickname);
      detail_handle.RegisterMember("their_nickname", &ContactDetail::their_nickname);
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
      detail_handle.RegisterMember("name_placeholder", &ContactDetail::name_placeholder);
      detail_handle.RegisterMember("has_relay_id", &ContactDetail::has_relay_id);
      detail_handle.RegisterMember("has_icon", &ContactDetail::has_icon);
      detail_handle.RegisterMember("icon_src", &ContactDetail::icon_src);
      detail_handle.RegisterMember("avatar_letter", &ContactDetail::avatar_letter);
      detail_handle.RegisterMember("avatar_tone", &ContactDetail::avatar_tone);
      detail_handle.RegisterMember("identities", &ContactDetail::identities);
      detail_handle.RegisterMember("threads", &ContactDetail::threads);
      detail_handle.RegisterMember("member_preview", &ContactDetail::member_preview);
      detail_handle.RegisterMember("starred", &ContactDetail::starred);
    }
    ctor.Bind("contacts", &controller.contacts_);
    ctor.Bind("contact_sections", &controller.contact_sections_);
    ctor.Bind("contact_index", &controller.contact_index_);
    ctor.Bind("contact_index_bubble", &controller.contact_index_bubble_);
    ctor.Bind("search_query", &controller.search_query_);
    ctor.Bind("add_query", &controller.add_query_);
    ctor.Bind("add_rows", &controller.add_rows_);
    ctor.Bind("add_searching", &controller.add_searching_);
    ctor.Bind("add_no_results", &controller.add_no_results_);
    ctor.Bind("my_has_account", &controller.my_has_account_);
    ctor.Bind("my_name", &controller.my_name_);
    ctor.Bind("my_account_id", &controller.my_account_id_);
    ctor.Bind("my_account_id_shown", &controller.my_account_id_shown_);
    ctor.Bind("my_has_icon", &controller.my_has_icon_);
    ctor.Bind("my_icon_src", &controller.my_icon_src_);
    ctor.Bind("my_avatar_letter", &controller.my_avatar_letter_);
    ctor.Bind("my_avatar_tone", &controller.my_avatar_tone_);
    ctor.Bind("my_qr_svg_rml", &controller.my_qr_svg_rml_);
    ctor.Bind("can_scan", &controller.can_scan_);
    ctor.Bind("can_new_group", &controller.can_new_group_);
    ctor.Bind("nc_frequent", &controller.new_chat_frequent_);
    ctor.Bind("nc_sections", &controller.new_chat_sections_);
    ctor.Bind("nc_query", &controller.new_chat_query_);
    ctor.Bind("blocked_rows", &controller.blocked_rows_);
    ctor.Bind("compact_layout", &controller.compact_layout_);
    ctor.Bind("advanced", &controller.advanced_);
    ctor.Bind("selected", &controller.selected_);
    ctor.BindEventCallback("select_contact", &ContactsController::SelectContactCallback);
    ctor.BindEventCallback("back_to_list", &ContactsController::BackToListCallback);
    ctor.BindEventCallback("start_chat", &ContactsController::StartChatCallback);
    ctor.BindEventCallback("secure_message", &ContactsController::SecureMessageCallback);
    ctor.BindEventCallback("add_member_contact", &ContactsController::AddMemberContactCallback);
    ctor.BindEventCallback("on_add_contact_menu", &ContactsController::AddContactMenuCallback);
    ctor.BindEventCallback("find_someone", &ContactsController::FindSomeoneCallback);
    ctor.BindEventCallback("copy_contact_id", &ContactsController::CopyIdCallback);
    ctor.BindEventCallback("share_contact", &ContactsController::ShareContactCallback);
    ctor.BindEventCallback("toggle_trust_friendly", &ContactsController::ToggleTrustFriendlyCallback);
    ctor.BindEventCallback("toggle_star", DataEvent<>([this]() { OnToggleStar(); }));
    ctor.BindEventCallback("block_contact", &ContactsController::BlockContactCallback);
    ctor.BindEventCallback("unblock_contact", &ContactsController::UnblockContactCallback);
    ctor.BindEventCallback("remove_contact", &ContactsController::RemoveContactCallback);
    ctor.BindEventCallback("open_thread", &ContactsController::OpenThreadCallback);
    ctor.BindEventCallback("on_search_changed", &ContactsController::OnSearchChangedCallback);
    ctor.BindEventCallback("on_contact_field_changed", &ContactsController::OnContactFieldChangedCallback);
    ctor.BindEventCallback("on_contact_field_left", DataEvent<>([this]() { OnContactFieldLeft(); }));
    ctor.Bind("admin_shown", &admin_shown_);
    ctor.Bind("admin_can_reinvite", &admin_can_reinvite_);
    ctor.Bind("admin_can_remove", &admin_can_remove_);
    ctor.Bind("admin_reinvite_note", &admin_reinvite_note_);
    ctor.BindEventCallback("admin_reinvite", DataEvent<>([this]() { OnGroupAdminReinvite(); }));
    ctor.BindEventCallback("admin_remove", DataEvent<>([this]() { OnGroupAdminRemove(); }));
    ctor.BindEventCallback("sync_contact_remote", &ContactsController::SyncRemoteCallback);
    ctor.BindEventCallback("add_search_changed", &ContactsController::AddSearchChangedCallback);
    ctor.BindEventCallback("add_pick", DataEvent<std::string>([this](const std::string& i) { OnAddPick(i); }));
    ctor.BindEventCallback("add_action", DataEvent<std::string>([this](const std::string& a) { OnAddAction(a); }));
    ctor.BindEventCallback("add_copy_id", DataEvent<>([this]() { OnAddCopyId(); }));
    ctor.BindEventCallback("add_share", DataEventAt([this](ui::Event& ev) { OnAddShare(ShareAnchorFromEvent(ev)); }));
    ctor.BindEventCallback("new_chat_pick", &ContactsController::NewChatPickCallback);
    ctor.BindEventCallback("new_chat_action", &ContactsController::NewChatActionCallback);
    ctor.BindEventCallback("new_chat_search_changed", &ContactsController::NewChatSearchCallback);
    ctor.BindEventCallback("blocked_unblock",
                           DataEvent<std::string>([this](const std::string& id) { OnUnblockListed(id); }));
    ctor.BindEventCallback("blocked_open_contact",
                           DataEvent<std::string>([this](const std::string& id) { OnOpenBlockedContact(id); }));
    ctor.BindEventCallback("contacts_index_at", DataEventAt([this](ui::Event& ev) { OnContactsIndexAt(ev); }));
    ctor.BindEventCallback("contacts_index_end", DataEventAt([this](ui::Event&) { OnContactsIndexEnd(); }));
  });

  return registered;
}

void ContactsController::SetShowAdvanced(const bool advanced) {
  if (advanced_ == advanced) {
    return;
  }
  advanced_ = advanced;
  DataModelHost::Instance().Dirty("contacts", "advanced");
}

void ContactsController::DirtyAll() {
  auto& host = DataModelHost::Instance();
  host.Dirty("contacts", "contacts");
  host.Dirty("contacts", "contact_sections");
  host.Dirty("contacts", "contact_index");
  host.Dirty("contacts", "contact_index_bubble");
  host.Dirty("contacts", "search_query");
  host.Dirty("contacts", "compact_layout");
  host.Dirty("contacts", "advanced");
  host.Dirty("contacts", "selected");
  host.Dirty("contacts", "admin_shown");
  host.Dirty("contacts", "admin_can_reinvite");
  host.Dirty("contacts", "admin_can_remove");
  host.Dirty("contacts", "admin_reinvite_note");
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
  contact_sections_.clear();
  contact_index_.clear();
  contact_index_bubble_ = "";
  if (!contacts_ports_.snapshot || !contacts_ports_.snapshot().initialized) {
    return;
  }

  const std::string query = util::Trim(search_query_.c_str());
  Roe<std::vector<Contact>> stored = contacts_ports_.list_contacts
                                         ? contacts_ports_.list_contacts()
                                         : Roe<std::vector<Contact>>::error(Error("contacts port unavailable"));
  if (!stored) {
    return;
  }

  const std::vector<std::string> starred = Starred();
  std::vector<ContactListRow> rows;
  rows.reserve(stored->size());
  for (const Contact& contact : *stored) {
    if (ContactMatchesQuery(contact, query)) {
      rows.push_back(ToContactListRow(contact, contacts_ports_));
      rows.back().starred = StarRank(starred, contact.id) >= 0;
    }
  }
  contact_sections_ = ContactSections(std::move(rows));
  contact_index_ = ContactIndexLetters(contact_sections_);
  for (const NewChatSectionRow& section : contact_sections_) {
    contacts_.insert(contacts_.end(), section.rows.begin(), section.rows.end());
  }
}

std::vector<ContactsController::NewChatSectionRow> ContactsController::ContactSections(std::vector<ContactListRow> rows) {
  std::stable_sort(rows.begin(), rows.end(), [](const ContactListRow& a, const ContactListRow& b) {
    return NameIndexLess(a.title.c_str(), b.title.c_str());
  });
  std::vector<NewChatSectionRow> sections;
  for (ContactListRow& row : rows) {
    const ui::String letter(1, NameIndexLetter(row.title.c_str()));
    if (sections.empty() || sections.back().letter != letter) {
      sections.push_back({letter, {}});
    }
    sections.back().rows.push_back(std::move(row));
  }
  return sections;
}

namespace {
constexpr std::string_view kIndexLetters = "ABCDEFGHIJKLMNOPQRSTUVWXYZ#";
}

std::vector<ContactsController::IndexLetterRow> ContactsController::ContactIndexLetters(
    const std::vector<NewChatSectionRow>& sections) {
  std::vector<IndexLetterRow> letters;
  for (const char letter : kIndexLetters) {
    const bool present = std::any_of(sections.begin(), sections.end(), [&](const NewChatSectionRow& section) {
      return section.letter.size() == 1 && section.letter[0] == letter;
    });
    letters.push_back({ui::String(1, letter), present});
  }
  return letters;
}

int ContactsController::ContactSectionForLetter(const std::vector<NewChatSectionRow>& sections, const char letter) {
  if (sections.empty()) {
    return -1;
  }
  const size_t wanted = kIndexLetters.find(letter);
  for (size_t i = 0; i < sections.size(); ++i) {
    const size_t at = sections[i].letter.empty() ? std::string_view::npos : kIndexLetters.find(sections[i].letter[0]);
    if (wanted != std::string_view::npos && at != std::string_view::npos && at >= wanted) {
      return static_cast<int>(i);
    }
  }
  return static_cast<int>(sections.size()) - 1;
}

void ContactsController::OnContactsIndexAt(ui::Event& ev) {
  ui::Element* strip = ev.GetCurrentElement();
  if (!strip) {
    return;
  }
  // The letter whose centre is nearest the finger (above the first or below the last picks that end).
  const float y = static_cast<float>(ev.GetParameter<int>("mouse_y", 0));
  ui::ElementList letters;
  strip->QuerySelectorAll(letters, ".contacts-index-letter");
  int nearest = -1;
  float nearest_distance = 0.f;
  for (size_t i = 0; i < letters.size() && i < kIndexLetters.size(); ++i) {
    const float centre = letters[i]->GetAbsoluteOffset(ui::BoxArea::Border).y +
                         letters[i]->GetBox().GetSize(ui::BoxArea::Border).y * 0.5f;
    const float distance = std::abs(centre - y);
    if (nearest < 0 || distance < nearest_distance) {
      nearest = static_cast<int>(i);
      nearest_distance = distance;
    }
  }
  if (nearest < 0) {
    return;
  }
  const char letter = kIndexLetters[static_cast<size_t>(nearest)];
  if (contact_index_bubble_ != ui::String(1, letter)) {
    contact_index_bubble_ = ui::String(1, letter);
    DataModelHost::Instance().Dirty("contacts", "contact_index_bubble");
  }

  // Scroll so the section's header sits just under the bar (the list view's top padding).
  const int section = ContactSectionForLetter(contact_sections_, letter);
  ui::Element* panel = strip->GetParentNode();
  ui::Element* view = panel ? panel->QuerySelector(".contacts-list-view") : nullptr;
  if (section < 0 || !view) {
    return;
  }
  ui::ElementList headers;
  view->QuerySelectorAll(headers, ".contacts-section-header");
  if (static_cast<size_t>(section) >= headers.size()) {
    return;
  }
  const float header_top = headers[static_cast<size_t>(section)]->GetAbsoluteOffset(ui::BoxArea::Border).y;
  const float view_top = view->GetAbsoluteOffset(ui::BoxArea::Border).y;
  const float under_bar = view->GetBox().GetEdge(ui::BoxArea::Padding, ui::BoxEdge::Top);
  const float max_top = std::max(0.f, view->Scroll().GetScrollHeight() - view->GetClientHeight());
  view->Scroll().SetScrollTop(std::clamp(view->Scroll().GetScrollTop() + header_top - view_top - under_bar, 0.f, max_top));
}

void ContactsController::OnContactsIndexEnd() {
  if (!contact_index_bubble_.empty()) {
    contact_index_bubble_ = "";
    DataModelHost::Instance().Dirty("contacts", "contact_index_bubble");
  }
}

void ContactsController::Refresh() {
  FlushPending();
  SyncFromStore();
  if (new_chat_open_) {
    SyncNewChat();
    DirtyNewChat();
  }
  if (blocked_open_) {
    SyncBlocked();
    DirtyBlocked();
  }
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

void ContactsController::SelectContactCallback(ui::DataModelHandle /*model*/, ui::Event& /*ev*/,
                                               const ui::VariantList& args) {
  if (args.empty() || args[0].GetType() != ui::Variant::STRING) {
    return;
  }
  Instance().OnSelectContact(std::string(args[0].Get<ui::String>().c_str()));
}

void ContactsController::BackToListCallback(ui::DataModelHandle /*model*/, ui::Event& /*ev*/,
                                            const ui::VariantList& /*args*/) {
  Instance().OnBackToList();
}

void ContactsController::StartChatCallback(ui::DataModelHandle /*model*/, ui::Event& /*ev*/,
                                           const ui::VariantList& /*args*/) {
  Instance().OnStartChat();
}

void ContactsController::SecureMessageCallback(ui::DataModelHandle /*model*/, ui::Event& /*ev*/,
                                               const ui::VariantList& /*args*/) {
  Instance().OnSecureMessage();
}

void ContactsController::AddMemberContactCallback(ui::DataModelHandle /*model*/, ui::Event& /*ev*/,
                                                  const ui::VariantList& /*args*/) {
  // The usual add-by-Account-ID path (directory lookup, keys from it, never from the group); once added, the page
  // turns into the contact's own (AddContactFromHit selects it).
  ContactsController& controller = Instance();
  if (controller.selected_.member_preview) {
    controller.AddContactByQuery(controller.selected_.member_account.c_str());
  }
}

void ContactsController::AddContactMenuCallback(ui::DataModelHandle /*model*/, ui::Event& ev,
                                              const ui::VariantList& /*args*/) {
  Instance().OnAddContactMenu(ev);
}

void ContactsController::FindSomeoneCallback(ui::DataModelHandle /*model*/, ui::Event& /*ev*/,
                                             const ui::VariantList& /*args*/) {
  Instance().OnFindSomeone();
}

void ContactsController::OnContactFieldChangedCallback(ui::DataModelHandle /*model*/, ui::Event& /*ev*/,
                                                         const ui::VariantList& /*args*/) {
  Instance().OnContactFieldChanged();
}

void ContactsController::SyncRemoteCallback(ui::DataModelHandle /*model*/, ui::Event& /*ev*/,
                                            const ui::VariantList& /*args*/) {
  Instance().OnSyncRemote();
}

void ContactsController::ToggleTrustFriendlyCallback(ui::DataModelHandle /*model*/, ui::Event& /*ev*/,
                                                     const ui::VariantList& /*args*/) {
  auto& controller = Instance();
  controller.OnSetTrust(controller.selected_.trust_key == "friendly" ? "unknown" : "friendly");
}

void ContactsController::BlockContactCallback(ui::DataModelHandle /*model*/, ui::Event& /*ev*/,
                                              const ui::VariantList& /*args*/) {
  auto& controller = Instance();
  const std::string name = controller.selected_.title.empty() ? Tr("contacts.this_contact")
                                                              : std::string(controller.selected_.title.c_str());
  controller.ShowConfirm(Tr("contacts.block_confirm_title"), Tr("contacts.block_confirm_body", {{"name", name}}),
                         [](const bool ok) {
                           if (ok) {
                             Instance().OnSetTrust("blocked");
                           }
                         }, true);
}

void ContactsController::UnblockContactCallback(ui::DataModelHandle /*model*/, ui::Event& /*ev*/,
                                                const ui::VariantList& /*args*/) {
  Instance().OnSetTrust("unknown");
}

void ContactsController::CopyIdCallback(ui::DataModelHandle /*model*/, ui::Event& /*ev*/,
                                        const ui::VariantList& /*args*/) {
  Instance().OnCopyId();
}

void ContactsController::ShareContactCallback(ui::DataModelHandle /*model*/, ui::Event& ev,
                                              const ui::VariantList& /*args*/) {
  Instance().OnShareContact(MenuPositionBelowEvent(ev));
}

void ContactsController::RemoveContactCallback(ui::DataModelHandle /*model*/, ui::Event& /*ev*/,
                                               const ui::VariantList& /*args*/) {
  Instance().OnRemoveContact();
}

void ContactsController::OpenThreadCallback(ui::DataModelHandle /*model*/, ui::Event& /*ev*/,
                                            const ui::VariantList& args) {
  if (args.empty() || args[0].GetType() != ui::Variant::STRING) {
    return;
  }
  Instance().OnOpenThread(std::string(args[0].Get<ui::String>().c_str()));
}

namespace {
/** The keyboard's search / return key arrives as a change event with linebreak set: it only dismisses the keyboard. */
void DismissKeyboardOnReturn(ui::Event& ev) {
  if (!ev.GetParameter<bool>("linebreak", false)) {
    return;
  }
  if (ui::Element* target = ev.GetTargetElement()) {
    target->Blur();
  }
  if (ui::SystemInterface* system = ui::GetSystemInterface()) {
    system->DeactivateKeyboard();
  }
}

// A change event runs before data-value has copied the field into the model, so the model still holds the text from one
// keystroke ago (Kenneth 2026-11-04: "输入了以后没什么反应"). The event carries the new text: take it from there.
void TakeEventValue(ui::Event& ev, ui::String& field) {
  const ui::String value = ev.GetParameter<ui::String>("value", field);
  if (value != field) {
    field = value;
  }
}
} // namespace

void ContactsController::OnSearchChangedCallback(ui::DataModelHandle /*model*/, ui::Event& ev,
                                                 const ui::VariantList& /*args*/) {
  auto& controller = Instance();
  TakeEventValue(ev, controller.search_query_);
  controller.OnSearchChanged();
  DismissKeyboardOnReturn(ev);
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
  selected_.starred = StarRank(Starred(), contact_id) >= 0;
  ApplyMessagingEligibility(selected_, **contact, contacts_ports_);
}

void ContactsController::UpdateMessagingEligibility(const Contact& contact) {
  ApplyMessagingEligibility(selected_, contact, contacts_ports_);
}

void ContactsController::OnSelectContact(const std::string& contact_id) {
  ClearGroupAdminCard();
  FlushPending();
  LoadSelectedDetail(contact_id);
  if (selected_.id.empty()) {
    return;
  }
  MaybeRefreshContactCard(contact_id);
  if (contacts_ports_.refresh_contact_profile) {
    contacts_ports_.refresh_contact_profile(contact_id); // a changed photo or name shows up; silent when fresh
  }

  OpenContactDetailPane();
  DirtyAll();
  NotifySurfaceChanged();
}

void ContactsController::OnShowMember(const std::string& account_id, const std::string& name,
                                      const std::string& icon_src) {
  ClearGroupAdminCard();
  if (account_id.empty()) {
    return;
  }
  FlushPending();
  selected_ = MemberPreviewDetail(account_id, name, icon_src);
  OpenContactDetailPane();
  DirtyAll();
  NotifySurfaceChanged();
}

void ContactsController::NoticeDetailSheetClosed() {
  if (detail_sheet_layer_id_ >= 0 &&
      !(shell_navigation_.has_layer && shell_navigation_.has_layer(detail_sheet_layer_id_))) {
    OnDetailSheetClosed();
  }
}

bool ContactsController::DetailSheetAvailable() const {
  return ChromeSnapshot().layout_mode == LayoutMode::Compact && shell_navigation_.push_layer &&
         shell_navigation_.has_layer;
}

void ContactsController::OnSelectContactSheet(const std::string& contact_id) {
  ClearGroupAdminCard();
  if (!DetailSheetAvailable()) {
    OnSelectContact(contact_id);
    return;
  }
  NoticeDetailSheetClosed();
  FlushPending();
  if (!DetailSheetOpen()) {
    detail_under_sheet_ = selected_.member_preview ? std::string() : std::string(selected_.id.c_str());
  }
  LoadSelectedDetail(contact_id);
  if (selected_.id.empty()) {
    return;
  }
  MaybeRefreshContactCard(contact_id);
  if (contacts_ports_.refresh_contact_profile) {
    contacts_ports_.refresh_contact_profile(contact_id);
  }
  OpenContactDetailSheet();
  DirtyAll();
  NotifySurfaceChanged();
}

void ContactsController::OnShowMemberSheet(const std::string& account_id, const std::string& name,
                                           const std::string& icon_src) {
  ClearGroupAdminCard();
  if (!DetailSheetAvailable()) {
    OnShowMember(account_id, name, icon_src);
    return;
  }
  if (account_id.empty()) {
    return;
  }
  NoticeDetailSheetClosed();
  FlushPending();
  if (!DetailSheetOpen()) {
    detail_under_sheet_ = selected_.member_preview ? std::string() : std::string(selected_.id.c_str());
  }
  selected_ = MemberPreviewDetail(account_id, name, icon_src);
  OpenContactDetailSheet();
  DirtyAll();
  NotifySurfaceChanged();
}

void ContactsController::OpenContactDetailSheet() {
  if (DetailSheetOpen()) {
    return;
  }
  PaneSpec spec;
  spec.key = "contact_detail";
  spec.page_sheet = true; // the sheets' one height, handle, centred title and grey X (Kenneth 2026-11-04)
  detail_sheet_layer_id_ = shell_navigation_.push_layer(spec);
}

void ContactsController::CloseSheetsBeforeLeaving() {
  if (shell_navigation_.close_page_sheets) {
    shell_navigation_.close_page_sheets(); // the whole stack (group info, member list, this page) goes at once
  }
  if (DetailSheetOpen()) {
    OnDetailSheetClosed();
  }
}

void ContactsController::SetGroupAdminCard(GroupAdminCard card) {
  group_admin_ = std::move(card);
  admin_can_reinvite_ = group_admin_.can_reinvite && group_admin_.reinvite;
  admin_can_remove_ = group_admin_.can_remove && group_admin_.remove;
  admin_reinvite_note_ = group_admin_.reinvite_note.c_str();
  admin_shown_ = admin_can_reinvite_ || admin_can_remove_;
  DirtyAll();
}

void ContactsController::ClearGroupAdminCard() {
  if (!admin_shown_ && !group_admin_.reinvite && !group_admin_.remove) {
    return;
  }
  group_admin_ = {};
  admin_shown_ = false;
  admin_can_reinvite_ = false;
  admin_can_remove_ = false;
  admin_reinvite_note_ = "";
  DirtyAll();
}

void ContactsController::OnGroupAdminReinvite() {
  if (admin_can_reinvite_ && group_admin_.reinvite) {
    group_admin_.reinvite();
  }
}

void ContactsController::OnGroupAdminRemove() {
  if (!admin_can_remove_ || !group_admin_.remove) {
    return;
  }
  // Asks first; once the person is out their page closes and the sheets under it stay (Kenneth 2026-11-04).
  const int layer = detail_sheet_layer_id_;
  const auto remove = group_admin_.remove;
  remove([this, layer]() {
    ClearGroupAdminCard();
    if (layer >= 0 && layer == detail_sheet_layer_id_) {
      if (shell_navigation_.close_layer) {
        shell_navigation_.close_layer(layer);
      }
      NoticeDetailSheetClosed();
    }
  });
}

void ContactsController::OnDetailSheetClosed() {
  ClearGroupAdminCard();
  detail_sheet_layer_id_ = -1;
  FlushPending();
  const std::string under = std::exchange(detail_under_sheet_, std::string());
  if (!under.empty()) {
    LoadSelectedDetail(under); // the Contacts tab's own page shows its contact again
  } else {
    selected_ = {};
    contact_dirty_ = false;
    debounce_deadline_ms_ = 0;
  }
  DirtyAll();
  NotifySurfaceChanged();
}

void ContactsController::MaybeRefreshContactCard(const std::string& contact_id) {
  // Not messageable yet: its card may never have arrived (its devices were out of reach when it was added,
  // #409). Opening it asks once more; the page updates when the card comes.
  if (selected_.can_message || !contacts_ports_.refresh_contact_card || !contacts_ports_.get_contact) {
    return;
  }
  auto contact = contacts_ports_.get_contact(contact_id);
  if (!contact || !*contact) {
    return;
  }
  contacts_ports_.refresh_contact_card(**contact, [](bool kept) {
    if (kept) {
      AppRuntime::PostUI([]() { ContactsController::Instance().Refresh(); });
    }
  });
}

void ContactsController::OpenContactDetailPane() {
  if (DetailSheetOpen()) {
    return; // the sheet shows the shared detail: a contact just added there (a member) stays in it
  }
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
  if (DetailSheetOpen()) {
    CloseSheetsBeforeLeaving(); // removed or blocked from the sheet: it goes, the page under it stays
    return true;
  }
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

void ContactsController::NewChatPickCallback(ui::DataModelHandle /*model*/, ui::Event& /*ev*/,
                                             const ui::VariantList& args) {
  if (args.empty() || args[0].GetType() != ui::Variant::STRING) {
    return;
  }
  Instance().OnNewChatPick(std::string(args[0].Get<ui::String>().c_str()));
}

void ContactsController::NewChatActionCallback(ui::DataModelHandle /*model*/, ui::Event& /*ev*/,
                                               const ui::VariantList& args) {
  if (args.empty() || args[0].GetType() != ui::Variant::STRING) {
    return;
  }
  Instance().OnNewChatAction(std::string(args[0].Get<ui::String>().c_str()));
}

void ContactsController::NewChatSearchCallback(ui::DataModelHandle /*model*/, ui::Event& ev,
                                               const ui::VariantList& /*args*/) {
  auto& controller = Instance();
  TakeEventValue(ev, controller.new_chat_query_);
  controller.SyncNewChat();
  controller.DirtyNewChat();
  DismissKeyboardOnReturn(ev);
}

void ContactsController::OpenNewChatPage() {
  if (new_chat_open_ || !shell_navigation_.push_layer) {
    return;
  }
  new_chat_query_ = "";
  SyncNewChat();
  new_chat_open_ = true;
  DataModelHost::Instance().Dirty("contacts", "nc_query");
  DirtyNewChat();
  // An iOS page sheet over the Chats list, like New group (Kenneth 2026-11-04): no back bar; X, scrim or swipe closes.
  PaneSpec spec;
  spec.key = "new_chat";
  spec.page_sheet = true;
  new_chat_layer_id_ = shell_navigation_.push_layer(spec);
  if (flow_coordinator_.begin_modal) {
    flow_coordinator_.begin_modal(new_chat_layer_id_, {}, [this]() { OnNewChatDismissed(); });
  }
}

void ContactsController::OnNewChatDismissed() {
  new_chat_open_ = false;
  new_chat_layer_id_ = -1;
}

void ContactsController::CloseNewChatPage() {
  if (!new_chat_open_) {
    return;
  }
  const int layer_id = new_chat_layer_id_;
  if (flow_coordinator_.end_modal) {
    flow_coordinator_.end_modal();
  }
  OnNewChatDismissed();
  if (layer_id >= 0 && shell_navigation_.close_layer) {
    shell_navigation_.close_layer(layer_id);
  }
}

void ContactsController::DirtyNewChat() {
  auto& host = DataModelHost::Instance();
  host.Dirty("contacts", "nc_frequent");
  host.Dirty("contacts", "nc_sections");
}

void ContactsController::SyncNewChat() {
  new_chat_frequent_.clear();
  new_chat_sections_.clear();
  if (!contacts_ports_.snapshot || !contacts_ports_.snapshot().initialized || !contacts_ports_.list_contacts) {
    return;
  }
  auto stored = contacts_ports_.list_contacts();
  if (!stored) {
    return;
  }
  const std::vector<std::string> starred = Starred();
  std::vector<NewChatContact> input;
  std::vector<ContactListRow> rows;
  for (const Contact& contact : *stored) {
    ContactListRow row = ToContactListRow(contact, contacts_ports_);
    row.starred = StarRank(starred, contact.id) >= 0;
    input.push_back({contact.id, row.title.c_str(), contact.trust == TrustLevel::Blocked, StarRank(starred, contact.id)});
    rows.push_back(std::move(row));
  }
  std::vector<NewChatDirectChat> chats;
  if (contacts_ports_.list_threads) {
    if (auto threads = contacts_ports_.list_threads()) {
      for (const Thread& thread : *threads) {
        if (thread.kind != ThreadKind::Direct || !thread.listed) {
          continue;
        }
        for (const std::string& contact_id : thread.participant_contact_ids) {
          chats.push_back({contact_id, thread.updated_at});
        }
      }
    }
  }
  const NewChatList list = BuildNewChatList(input, chats, util::Trim(new_chat_query_.c_str()));
  for (const size_t i : list.frequent) {
    new_chat_frequent_.push_back(rows[i]);
  }
  for (const NewChatSection& section : list.sections) {
    NewChatSectionRow out;
    out.letter = section.letter.c_str();
    for (const size_t i : section.contacts) {
      out.rows.push_back(rows[i]);
    }
    new_chat_sections_.push_back(std::move(out));
  }
}

// A contact chosen on the New chat page opens (or creates) the same chat the contact page's "Message" does.
void ContactsController::OnNewChatPick(const std::string& contact_id) {
  LoadSelectedDetail(contact_id);
  if (selected_.id.empty()) {
    return;
  }
  if (!selected_.can_message) {
    if (!selected_.message_hint.empty()) { // the demo app has no hint: no empty pill
      ShowToast(selected_.message_hint.c_str()); // the page stays; the person cannot be messaged yet
    }
    selected_ = {};
    return;
  }
  CloseNewChatPage();
  OnStartChat();
  selected_ = {};
  DirtyAll();
}

void ContactsController::OnNewChatAction(const std::string& action) {
  CloseNewChatPage();
  if (action == "ai") {
    if (chat_ports_.new_ai_chat) {
      chat_ports_.new_ai_chat();
    }
  } else if (action == "group") {
    OnNewGroup();
  } else if (action == "add") {
    OnAddContact();
  }
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
  remark_notice_deadline_ms_ = SDL_GetTicks() + kRemarkNoticeQuietMs;
  WakeForRemarkNotice();

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
  NoticeDetailSheetClosed();
  if (add_search_deadline_ms_ != 0 && SDL_GetTicks() >= add_search_deadline_ms_) {
    add_search_deadline_ms_ = 0;
    RunAddSearch();
  }
  if (contact_dirty_ && debounce_deadline_ms_ != 0 && SDL_GetTicks() >= debounce_deadline_ms_) {
    debounce_deadline_ms_ = 0;
    (void)FlushSelectedContact();
  }
  // A pause in the typing ends the edit (a tap elsewhere on a phone does not always take the focus from the field).
  if (remark_notice_deadline_ms_ != 0 && !contact_dirty_ && SDL_GetTicks() >= remark_notice_deadline_ms_) {
    AnnounceRemarkSaved();
  }
}

void ContactsController::WakeForRemarkNotice() {
  // An idle phone draws no frames, so nothing would tick when the pause is over: wake the loop then.
  if (remark_wake_pending_ || remark_notice_deadline_ms_ == 0 || !AppRuntime::IsRunning()) {
    return;
  }
  remark_wake_pending_ = true;
  const uint64_t now = SDL_GetTicks();
  const uint64_t delay = remark_notice_deadline_ms_ > now ? remark_notice_deadline_ms_ - now : 0;
  std::thread([this, delay]() {
    std::this_thread::sleep_for(std::chrono::milliseconds(delay + 20));
    AppRuntime::PostUI([this]() {
      remark_wake_pending_ = false;
      Tick();
      WakeForRemarkNotice(); // typing went on meanwhile: wait for the new pause
    });
  }).detach();
}

void ContactsController::AnnounceRemarkSaved() {
  remark_notice_deadline_ms_ = 0;
  if (const std::string notice = remark_notice_.Take(); !notice.empty()) {
    ShowToast(Tr(notice));
  }
}

void ContactsController::FlushPending() {
  if (contact_dirty_) {
    debounce_deadline_ms_ = 0;
    (void)FlushSelectedContact();
  }
  // Leaving the field or the page (closed, another person, back) also ends the edit.
  AnnounceRemarkSaved();
}

void ContactsController::OnContactFieldLeft() {
  FlushPending();
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
  const bool remark_changed = updated.local.display_name != (*existing)->local.display_name;
  if (!contacts_ports_.upsert_contact(updated)) {
    remark_notice_.OnSaved(remark_changed, false);
    UserFeedback::Fail(Tr(remark_changed ? "contacts.remark.save_failed" : "contacts.err.save"));
    return false;
  }
  remark_notice_.OnSaved(remark_changed, true);

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

// The contact already holding this account id, if any (scans the store's list; no separate index exists).
std::optional<Contact> ContactsController::FindContactByAccount(const std::string& account_id) const {
  if (account_id.empty() || !contacts_ports_.list_contacts) {
    return std::nullopt;
  }
  auto all = contacts_ports_.list_contacts();
  if (!all) {
    return std::nullopt;
  }
  for (const Contact& contact : *all) {
    if (ContactAccountId(contact) == account_id) {
      return contact;
    }
  }
  return std::nullopt;
}

// Already in the contacts: say so and show them; nothing is added or changed.
void ContactsController::ShowAlreadyContact(const Contact& contact) {
  SyncFromStore();
  OnSelectContact(contact.id);
  const std::string name = !contact.display_name.empty() ? contact.display_name : contact.server_nickname;
  ShowToast(Tr("contacts.already_contact", {{"name", name}}));
  NotifySurfaceChanged();
}

std::vector<ContextMenuAction> ContactsController::AddContactMenuActions() {
  return {
      {
          "add_contact",
          Tr("contacts.add"),
          nullptr,
          []() { ContactsController::Instance().OnAddContact(); },
          "../icons/v2/user-plus.svg",
      },
      {
          "find_someone",
          Tr("contacts.find_someone"),
          nullptr,
          []() { ContactsController::Instance().OnFindSomeone(); },
          "../icons/v2/search.svg",
      },
  };
}

void ContactsController::OnAddContactMenu(ui::Event& ev) {
  if (compact_layout_) {
    OpenAddContactPage(); // the phone "+": a page sheet like New chat, not a menu
    return;
  }
  ContextMenuHost::Instance().ShowActions(MenuPositionBelowEvent(ev), AddContactMenuActions());
}

void ContactsController::OnNewGroup() {
  if (chat_ports_.open_new_group) {
    chat_ports_.open_new_group();
  }
}

void ContactsController::OnScanQr() {
  if (!scan_ports_.scan_qr) {
    return;
  }
  scan_ports_.scan_qr([this](std::string text) {
    const bool valid = static_cast<bool>(FindContactLocator(text));
    MetricsLine("scan.contact").Add("valid_locator", valid).Emit();
    if (!valid) {
      ShowToast(Tr("scan.not_contact"), ToastDuration::Long);
      return;
    }
    OfferInvite(text);
  });
}

// "Add contact" asks the directory first: an account id or nickname is looked up and the person is
// added from the hit; no empty contacts (Kenneth 2026-10-23, TODO #33).
void ContactsController::OnAddContact() {
  if (!contacts_ports_.snapshot || !contacts_ports_.snapshot().initialized || !contacts_ports_.search_people ||
      !contacts_ports_.add_contact_from_directory_hit || !shell_feedback_.show_prompt) {
    return;
  }
  shell_feedback_.show_prompt(Tr("contacts.add_by_id_title"), Tr("contacts.add_by_id_message"), "",
                              [](const bool confirmed, std::string value) {
                                if (confirmed) {
                                  ContactsController::Instance().AddContactByQuery(util::Trim(value));
                                }
                              });
}

void ContactsController::AddContactByQuery(const std::string& query) {
  if (query.empty()) {
    return;
  }
  // A pasted invite names the person and their devices: no directory needed (ACCOUNT_CARD.md).
  if (query.find(kContactLocatorPrefix) != std::string::npos && contacts_ports_.add_from_locator) {
    if (auto locator = FindContactLocator(query)) {
      if (auto known = FindContactByAccount(locator->account_id)) {
        ShowAlreadyContact(*known);
        return;
      }
    }
    contacts_ports_.add_from_locator(query, [](Roe<std::pair<std::string, bool>> added) {
      AppRuntime::PostUI([added = std::move(added)]() { ContactsController::Instance().OnAddedFromInvite(added); });
    });
    return;
  }
  if (!contacts_ports_.search_people) {
    return;
  }
  // The directory is an HTTP call: off the UI thread, the outcome comes back here.
  AppRuntime::PostWorkerAndReplyOnUI<Roe<std::vector<mesh::DirectoryHit>>>(
      WorkerLane::Normal, [search = contacts_ports_.search_people, query]() { return search(query); },
      [query](Roe<std::vector<mesh::DirectoryHit>> hits) {
        ContactsController::Instance().OnPeopleSearched(query, std::move(hits));
      });
}

void ContactsController::OfferInvite(const std::string& link) {
  auto locator = FindContactLocator(link);
  if (!locator) {
    return;
  }
  if (auto known = FindContactByAccount(locator->account_id)) {
    if (shell_navigation_.select_nav_tab) {
      shell_navigation_.select_nav_tab(NavTab::Contacts);
    }
    ShowAlreadyContact(*known);
    return;
  }
  const std::string name = locator->nickname.empty() ? locator->account_id : locator->nickname;
  auto ask = [this, link, name, account_id = locator->account_id]() {
    ShowConfirm(Tr("contacts.invite.open_title"),
                Tr("contacts.invite.open_body", {{"name", name}, {"account", account_id}}),
                [this, link](const bool ok) {
                  if (!ok) {
                    return;
                  }
                  if (shell_navigation_.select_nav_tab) {
                    shell_navigation_.select_nav_tab(NavTab::Contacts);
                  }
                  AddContactByQuery(link);
                });
  };
  if (!unlock_ensure_.ensure_unlocked) {
    ask();
    return;
  }
  unlock_ensure_.ensure_unlocked([ask = std::move(ask)](const bool unlocked) {
    if (unlocked) {
      ask();
    }
  });
}

void ContactsController::OnAddedFromInvite(const Roe<std::pair<std::string, bool>>& added) {
  if (!added) {
    UserFeedback::Fail(added.error().message.empty() ? Tr("contacts.err.add") : added.error().message);
    return;
  }
  SyncFromStore();
  OnSelectContact(added.value().first);
  // Without keys yet the contact stands; their first message brings them (or they come online).
  ShowToast(Tr(added.value().second ? "contacts.invite.added" : "contacts.invite.added_pending"));
  NotifySurfaceChanged();
}

void ContactsController::OnPeopleSearched(const std::string& query, Roe<std::vector<mesh::DirectoryHit>> hits) {
  if (!hits) {
    // Not-found never arrives as an error: SearchDirectoryPeople turns it into an empty hit list.
    if (ClassifyPeopleLookupError(hits.error()) == PeopleLookupFailure::DirectoryUnreachable) {
      UserFeedback::Fail(Tr("contacts.err.directory_unreachable"));
    } else {
      log().warning << "People lookup failed: " << AppError::Log(hits.error());
      UserFeedback::Fail(Tr("contacts.err.add"));
    }
    return;
  }
  if (hits->empty()) {
    UserFeedback::Fail(Tr("contacts.err.user_not_found"));
    return;
  }
  if (hits->size() == 1) {
    OfferHit(hits->front(), query);
    return;
  }
  // Several people match: pick one (name · short id). The hits are ranked, exact matches first,
  // so the first ten are the ones worth offering.
  constexpr size_t kMaxChoices = 10;
  std::vector<ContextMenuAction> actions;
  for (size_t i = 0; i < hits->size() && i < kMaxChoices; ++i) {
    const mesh::DirectoryHit& hit = (*hits)[i];
    const std::string name = !hit.display_name.empty()
                                 ? hit.display_name
                                 : (!hit.nickname.empty() ? hit.nickname : Tr("people.result.unknown_person"));
    const std::string id = PrimaryAccountIdFromHit(hit).value_or(PrimaryRelayIdFromHit(hit).value_or(""));
    const std::string label = id.empty() ? name : name + " · " + ShortRelayId(id);
    actions.push_back({"add_hit_" + hit.hit_id, label, nullptr,
                       [hit]() {
                         ContactsController& self = ContactsController::Instance();
                         const std::string account = PrimaryAccountIdFromHit(hit).value_or("");
                         if (std::optional<Contact> known = account.empty() ? std::nullopt : self.FindContactByAccount(account)) {
                           self.ShowAlreadyContact(*known);
                           return;
                         }
                         self.AddContactFromHit(hit);
                       },
                       "../icons/contacts.svg"});
  }
  actions.push_back({"add_cancel", Tr("common.cancel"), nullptr, []() {}, "../icons/v2/close.svg"});
  actions.back().cancel = true; // the phone sheet's close disc stands for it
  ContextMenuHost::Instance().ShowActions(ui::Vector2i(160, 120), std::move(actions), {}, Tr("contacts.add"));
}

void ContactsController::OfferHit(const mesh::DirectoryHit& hit, const std::string& query) {
  const std::string account = PrimaryAccountIdFromHit(hit).value_or("");
  // A relay-only hit has no account id: show its relay id in the dialog and skip the known-contact check.
  const std::string shown_id = !account.empty() ? account : PrimaryRelayIdFromHit(hit).value_or("");
  std::optional<Contact> known = account.empty() ? std::nullopt : FindContactByAccount(account);
  switch (DecideAddFromSearch(HitMatchesExactPeopleId(hit, query), known.has_value())) {
  case AddFromSearchDecision::AlreadyContact:
    ShowAlreadyContact(*known);
    return;
  case AddFromSearchDecision::ConfirmByName: {
    const std::string name = !hit.display_name.empty() ? hit.display_name : hit.nickname;
    ShowConfirm(Tr("contacts.confirm_add_title"),
                Tr("contacts.confirm_add_body", {{"name", name}, {"account", ShortRelayId(shown_id)}}),
                [hit](const bool ok) {
                  if (ok) {
                    ContactsController::Instance().AddContactFromHit(hit);
                  }
                });
    return;
  }
  case AddFromSearchDecision::AddDirect:
    AddContactFromHit(hit);
    return;
  }
}

void ContactsController::AddContactFromHit(const mesh::DirectoryHit& hit) {
  if (!contacts_ports_.add_contact_from_directory_hit) {
    return;
  }
  // Same steps as ContactActionDispatcher's add_contact: keys first, then the contact, then its direct endpoints.
  if (contacts_ports_.register_peer_keys_from_directory) {
    contacts_ports_.register_peer_keys_from_directory(hit);
  }
  auto created = contacts_ports_.add_contact_from_directory_hit(hit);
  if (!created) {
    UserFeedback::Fail(created.error().message.empty() ? Tr("contacts.err.add") : created.error().message);
    return;
  }
  if (contacts_ports_.register_contact_direct_endpoints) {
    contacts_ports_.register_contact_direct_endpoints(*created);
  }
  if (contacts_ports_.ensure_directory_hit_icon_cached) {
    contacts_ports_.ensure_directory_hit_icon_cached(hit);
  }
  SyncFromStore();
  OnSelectContact(created->id);
  DirtyAll();
  NotifySurfaceChanged();
  const std::string name = !created->display_name.empty() ? created->display_name : hit.nickname;
  ShowToast(Tr("contacts.added", {{"name", name}}).c_str());
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
      ShowToast(Tr("people_picker.pin_required"));
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
  std::string label = Tr("contacts.id_kind.relay");
  if (value.empty()) {
    value = selected_.peer_id.c_str();
    label = Tr("contacts.id_kind.peer");
  }
  if (value.empty() && !selected_.identities.empty()) {
    value = selected_.identities.front().value.c_str();
    label = Tr("contacts.id_kind.generic");
  }
  if (value.empty()) {
    ShowToast(Tr("contacts.toast.no_id"));
    return;
  }
  if (ui::SystemInterface* system = ui::GetSystemInterface()) {
    system->SetClipboardText(value.c_str());
  }
  ShowToast(Tr("contacts.toast.id_copied", {{"label", label}}));
}

// Share: the contact's card into one of your chats (ACCOUNT_CARD.md) — the person can add them with no
// directory; the direct chats, most recent first, not the contact's own.
void ContactsController::OnShareContact(const ui::Vector2i position) {
  if (selected_.id.empty() || !contacts_ports_.list_threads || !contacts_ports_.share_contact) {
    return;
  }
  constexpr size_t kMaxShareTargets = 30;
  const std::string contact_id = selected_.id.c_str();
  std::vector<Thread> targets;
  if (auto threads = contacts_ports_.list_threads()) {
    for (const Thread& thread : *threads) {
      const bool own_chat = std::find(thread.participant_contact_ids.begin(), thread.participant_contact_ids.end(),
                                      contact_id) != thread.participant_contact_ids.end();
      if (thread.kind == ThreadKind::Direct && !own_chat && !(thread.local_title.empty() && thread.title.empty())) {
        targets.push_back(thread);
      }
    }
  }
  std::sort(targets.begin(), targets.end(), [](const Thread& a, const Thread& b) { return a.updated_at > b.updated_at; });
  if (targets.size() > kMaxShareTargets) {
    targets.resize(kMaxShareTargets);
  }
  if (targets.empty()) {
    ShowToast(Tr("contacts.share.no_chats"));
    return;
  }
  std::vector<ContextMenuAction> actions;
  for (const Thread& thread : targets) {
    const std::string name = thread.local_title.empty() ? thread.title : thread.local_title;
    actions.push_back({"share_contact_to_" + thread.id, name, nullptr,
                       [thread_id = thread.id, contact_id, name]() {
                         ContactsController::Instance().ShareContactTo(thread_id, contact_id, name);
                       }});
  }
  ContextMenuHost::Instance().ShowActions(position, std::move(actions), {}, Tr("common.share"));
}

void ContactsController::ShareContactCard(const std::function<Roe<void>()>& share,
                                          const std::function<void(std::function<void(bool kept)>)>& refresh_card,
                                          std::function<void(bool shared)> done) {
  if (share()) {
    done(true);
    return;
  }
  if (!refresh_card) {
    done(false);
    return;
  }
  refresh_card([share, done = std::move(done)](const bool kept) {
    if (!kept) {
      done(false);
      return;
    }
    done(static_cast<bool>(share()));
  });
}

// The card a share sends is the contact's signed keys; a contact added while its devices were out of reach (and
// not in the directory) has none on this device yet (#409). Ask for it once, then say so in plain words.
void ContactsController::ShareContactTo(const std::string& thread_id, const std::string& contact_id,
                                        const std::string& name) {
  if (!contacts_ports_.share_contact) {
    return;
  }
  const auto share = [share = contacts_ports_.share_contact, thread_id, contact_id]() {
    auto sent = share(thread_id, contact_id);
    if (!sent) {
      ContactsController::Instance().log().info << "share contact: " << sent.error().message;
    }
    return sent;
  };
  std::function<void(std::function<void(bool)>)> refresh_card;
  if (contacts_ports_.refresh_contact_card && contacts_ports_.get_contact) {
    auto contact = contacts_ports_.get_contact(contact_id);
    if (contact && *contact) {
      refresh_card = [refresh = contacts_ports_.refresh_contact_card, found = **contact](std::function<void(bool)> kept) {
        // The answer comes on a worker or I/O thread; the retry and the toast belong on the UI thread.
        refresh(found, [kept = std::move(kept)](const bool got) { AppRuntime::PostUI([kept, got]() { kept(got); }); });
      };
    }
  }
  ShareContactCard(share, refresh_card, [name](const bool shared) {
    if (shared) {
      ContactsController::Instance().ShowToast(Tr("contacts.share.sent", {{"name", name}}));
      return;
    }
    UserFeedback::Fail(Tr("contacts.share.not_yet"));
  });
}

bool ContactsController::SetContactTrust(const std::string& contact_id, const std::string& trust) {
  if (!contacts_ports_.snapshot || !contacts_ports_.snapshot().initialized || contact_id.empty() ||
      !contacts_ports_.get_contact || !contacts_ports_.upsert_contact) {
    return false;
  }
  auto contact = contacts_ports_.get_contact(contact_id);
  if (!contact || !*contact) {
    return false;
  }
  Contact updated = **contact;
  updated.local.trust = TrustLevelFromString(trust);
  SyncContactMirrors(updated);
  if (!contacts_ports_.upsert_contact(updated)) {
    UserFeedback::Fail(Tr("contacts.err.trust"));
    return false;
  }
  return true;
}

void ContactsController::OnToggleStar() {
  const std::string contact_id = selected_.id.c_str();
  if (!session_store_ || contact_id.empty() || selected_.member_preview) {
    return;
  }
  // Kept on this device only (preferences.json), like the chats' pin / mute: never synced or sent.
  ProfilePreferences prefs = session_store_->Snapshot().profile_prefs;
  prefs.schema_version = ProfilePreferences::kSchemaVersion;
  prefs.starred_contacts = ToggleStarred(prefs.starred_contacts, contact_id);
  if (auto saved = session_store_->SaveProfilePrefs(prefs); !saved) {
    log().warning << "Contact star not saved: " << saved.error().message;
    ShowToast(UserFeedback::FailFrom(saved.error()));
    return;
  }
  selected_.starred = StarRank(prefs.starred_contacts, contact_id) >= 0;
  SyncFromStore(); // the list's star
  if (new_chat_open_) {
    SyncNewChat();
    DirtyNewChat();
  }
  DirtyAll();
}

void ContactsController::OnSetTrust(const std::string& trust) {
  const bool was_blocked = selected_.trust_key == "blocked";
  const std::string name = selected_.title.empty() ? Tr("contacts.this_contact") : std::string(selected_.title.c_str());
  if (!SetContactTrust(selected_.id.c_str(), trust)) {
    return;
  }
  LoadSelectedDetail(selected_.id.c_str());
  SyncFromStore();
  if (blocked_open_) {
    SyncBlocked(); // unblocked on a contact page opened off the blocked list: gone from it on the way back
    DirtyBlocked();
  }
  DirtyAll();
  if (trust == "blocked") {
    ShowToast(Tr("contacts.toast.blocked", {{"name", name}}));
  } else if (was_blocked) {
    ShowToast(Tr("contacts.toast.unblocked", {{"name", name}}));
  } else {
    ShowToast(Tr("contacts.toast.trust_updated"));
  }
  if (contacts_ports_.notify_thread_changed) {
    contacts_ports_.notify_thread_changed(); // the Chats list marks (or unmarks) the person's chat
  }
  NotifySurfaceChanged();
}

std::vector<Contact> BlockedContactsByName(const std::vector<Contact>& contacts) {
  std::vector<Contact> blocked;
  for (const Contact& contact : contacts) {
    if (contact.trust == TrustLevel::Blocked) {
      blocked.push_back(contact);
    }
  }
  std::stable_sort(blocked.begin(), blocked.end(), [](const Contact& a, const Contact& b) {
    return NameIndexLess(ContactEffectiveTitle(a), ContactEffectiveTitle(b));
  });
  return blocked;
}

void ContactsController::SyncBlocked() {
  blocked_rows_.clear();
  if (!contacts_ports_.snapshot || !contacts_ports_.snapshot().initialized || !contacts_ports_.list_contacts) {
    return;
  }
  auto stored = contacts_ports_.list_contacts();
  if (!stored) {
    return;
  }
  for (const Contact& contact : BlockedContactsByName(*stored)) {
    blocked_rows_.push_back(ToContactListRow(contact, contacts_ports_));
  }
}

void ContactsController::DirtyBlocked() {
  DataModelHost::Instance().Dirty("contacts", "blocked_rows");
}

int ContactsController::BlockedCount() const {
  if (!contacts_ports_.snapshot || !contacts_ports_.snapshot().initialized || !contacts_ports_.list_contacts) {
    return 0;
  }
  auto stored = contacts_ports_.list_contacts();
  return stored ? static_cast<int>(BlockedContactsByName(*stored).size()) : 0;
}

void ContactsController::OpenBlockedPeople() {
  if (blocked_open_ || !shell_navigation_.push_transient) {
    return;
  }
  FlushPending();
  SyncBlocked();
  blocked_open_ = true;
  DirtyBlocked();
  PaneSpec spec;
  spec.key = "blocked_people";
  shell_navigation_.push_transient(spec);
}

void ContactsController::OnBlockedPeopleDismissed() {
  blocked_open_ = false;
  blocked_rows_.clear();
}

void ContactsController::OnUnblockListed(const std::string& contact_id) {
  std::string name;
  for (const ContactListRow& row : blocked_rows_) {
    if (row.id == contact_id.c_str()) {
      name = row.title.c_str();
    }
  }
  if (name.empty() || !SetContactTrust(contact_id, "unknown")) {
    return;
  }
  // The row goes in place and the bottom pill says who was unblocked.
  SyncBlocked();
  DirtyBlocked();
  ShowToast(Tr("contacts.blocked.unblocked", {{"name", name}}));
  SyncFromStore();
  DirtyAll();
  if (contacts_ports_.notify_thread_changed) {
    contacts_ports_.notify_thread_changed();
  }
  NotifySurfaceChanged();
}

void ContactsController::OnOpenBlockedContact(const std::string& contact_id) {
  FlushPending();
  LoadSelectedDetail(contact_id);
  if (selected_.id.empty() || !shell_navigation_.push_transient) {
    return;
  }
  // Pushed over the list (not through OpenContactDetailPane: the Settings tab keeps its page under both).
  PaneSpec spec;
  spec.key = "contact_detail";
  shell_navigation_.push_transient(spec);
  DirtyAll();
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
    UserFeedback::Fail(Tr("contacts.err.no_relay_id"));
    return;
  }
  auto hit = contacts_ports_.lookup_relay_user(relay_id);
  if (!hit) {
    UserFeedback::Fail(hit.error().message.empty() ? Tr("contacts.err.sync") : hit.error().message);
    return;
  }
  const bool carries_keys = (hit->signing_public_key_b64 && !hit->signing_public_key_b64->empty()) ||
                            (hit->kem_public_key_b64 && !hit->kem_public_key_b64->empty());
  if (carries_keys && (!hit->account_id || hit->account_id->empty())) {
    UserFeedback::Fail(Tr("contacts.err.missing_account_id"));
    return;
  }
  if (contacts_ports_.register_peer_keys_from_directory) {
    contacts_ports_.register_peer_keys_from_directory(*hit);
  }
  auto applied = contacts_ports_.apply_remote_snapshot(selected_.id.c_str(), *hit, util::NowUnixMs());
  if (!applied) {
    UserFeedback::Fail(applied.error().message.empty() ? Tr("contacts.err.save_synced")
                                                       : applied.error().message);
    return;
  }
  if (contacts_ports_.register_contact_direct_endpoints) {
    contacts_ports_.register_contact_direct_endpoints(*applied);
  }
  LoadSelectedDetail(selected_.id.c_str());
  SyncFromStore();
  DirtyAll();
  ShowToast(Tr("contacts.toast.synced"));
  NotifySurfaceChanged();
}

void ContactsController::OnRemoveContact() {
  if (!contacts_ports_.snapshot || !contacts_ports_.snapshot().initialized || selected_.id.empty() ||
      !contacts_ports_.remove_contact) {
    return;
  }

  const std::string contact_id = selected_.id.c_str();
  const std::string display_name = selected_.display_name.empty() ? Tr("contacts.this_contact") : selected_.display_name.c_str();
  const std::string message = Tr("contacts.remove_confirm_body", {{"name", display_name}});

  ShowConfirm(Tr("contacts.remove"), message, [this, contact_id](const bool ok) {
    if (!ok) {
      return;
    }
    auto removed = contacts_ports_.remove_contact(contact_id);
    if (!removed) {
      UserFeedback::Fail(Tr("contacts.err.remove"));
      return;
    }
    if (!*removed) {
      ShowToast(Tr("contacts.err.not_found"));
      return;
    }
    if (!CloseContactDetailPane()) {
      OnDetailDismissed();
    }
    SyncFromStore();
    DirtyAll();
    ShowToast(Tr("contacts.toast.removed"));
    if (contacts_ports_.notify_thread_changed) {
      contacts_ports_.notify_thread_changed();
    }
    NotifySurfaceChanged();
  }, true);
}

void ContactsController::OnOpenThread(const std::string& thread_id) {
  if (thread_id.empty()) {
    return;
  }
  CloseSheetsBeforeLeaving();
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

// The phone list renders contact_sections and the index strip, not `contacts`: all of them change with the filter.
void ContactsController::OnSearchChanged() {
  SyncFromStore();
  auto& host = DataModelHost::Instance();
  host.Dirty("contacts", "contacts");
  host.Dirty("contacts", "contact_sections");
  host.Dirty("contacts", "contact_index");
  host.Dirty("contacts", "contact_index_bubble");
  host.Dirty("contacts", "search_query");
}

// ---- The phone Add contact sheet (Kenneth 2026-11-04: the "+" opens a big sheet, not a small menu) ----

namespace {
constexpr uint32_t kAddSearchDebounceMs = 300;
constexpr size_t kAddMaxHits = 10;
} // namespace

void ContactsController::OpenAddContactPage() {
  if (add_open_ || !shell_navigation_.push_layer) {
    return;
  }
  add_query_ = "";
  add_rows_.clear();
  add_hits_.clear();
  add_searching_ = false;
  add_no_results_ = false;
  add_search_deadline_ms_ = 0;
  ++add_search_generation_;
  LoadMyAccount();
  can_scan_ = static_cast<bool>(scan_ports_.scan_qr);
  can_new_group_ = static_cast<bool>(chat_ports_.open_new_group);
  add_open_ = true;
  DirtyAddContact();
  PaneSpec spec;
  spec.key = "add_contact";
  spec.page_sheet = true;
  add_layer_id_ = shell_navigation_.push_layer(spec);
  if (flow_coordinator_.begin_modal) {
    flow_coordinator_.begin_modal(add_layer_id_, {}, [this]() { OnAddContactDismissed(); });
  }
}

void ContactsController::OnAddContactDismissed() {
  add_open_ = false;
  add_layer_id_ = -1;
  add_search_deadline_ms_ = 0;
  ++add_search_generation_; // a search still in flight answers into nothing
}

void ContactsController::CloseAddContactPage() {
  if (!add_open_) {
    return;
  }
  const int layer_id = add_layer_id_;
  if (flow_coordinator_.end_modal) {
    flow_coordinator_.end_modal();
  }
  OnAddContactDismissed();
  if (layer_id >= 0 && shell_navigation_.close_layer) {
    shell_navigation_.close_layer(layer_id);
  }
}

void ContactsController::DirtyAddContact() {
  auto& host = DataModelHost::Instance();
  for (const char* name :
       {"add_query", "add_rows", "add_searching", "add_no_results", "my_has_account", "my_name", "my_account_id", "my_account_id_shown",
        "my_has_icon", "my_icon_src", "my_avatar_letter", "my_avatar_tone", "my_qr_svg_rml", "can_scan",
        "can_new_group"}) {
    host.Dirty("contacts", name);
  }
}

void ContactsController::LoadMyAccount() {
  const ProfileIdentityView view = account_ports_.load_identity ? account_ports_.load_identity() : ProfileIdentityView{};
  my_has_account_ = view.ready && !view.relay_id.empty();
  my_nickname_ = view.nickname;
  my_locator_ = view.contact_locator;
  const std::string name = view.nickname.empty() ? Tr("people_picker.unnamed") : view.nickname;
  my_name_ = name.c_str();
  my_account_id_ = view.relay_id.c_str();
  // Shown without the internal "relay:" / "account:" kind prefix; copy and share keep the full id.
  std::string shown_id = view.relay_id;
  for (const char* prefix : {"relay:", "account:"}) {
    if (shown_id.rfind(prefix, 0) == 0) {
      shown_id.erase(0, std::char_traits<char>::length(prefix));
      break;
    }
  }
  my_account_id_shown_ = shown_id.c_str();
  my_has_icon_ = view.profile_has_icon && !view.profile_icon_path.empty();
  my_icon_src_ = view.profile_icon_path.c_str();
  const AvatarGlyph glyph = MakeAvatarGlyph(name, AvatarStableId(view.account_id, view.relay_id, view.peer_id));
  my_avatar_letter_ = glyph.letter.c_str();
  my_avatar_tone_ = glyph.tone;
}

void ContactsController::AddSearchChangedCallback(ui::DataModelHandle /*model*/, ui::Event& ev,
                                                  const ui::VariantList& /*args*/) {
  TakeEventValue(ev, Instance().add_query_);
  Instance().OnAddSearchChanged();
  DismissKeyboardOnReturn(ev);
}

// Typing searches as you type: each keystroke restarts a 300 ms wait, then the directory is asked once.
void ContactsController::OnAddSearchChanged() {
  ++add_search_generation_;
  add_rows_.clear();
  add_hits_.clear();
  add_no_results_ = false;
  if (util::Trim(add_query_.c_str()).empty()) {
    add_searching_ = false;
    add_search_deadline_ms_ = 0;
  } else {
    add_searching_ = true;
    add_search_deadline_ms_ = SDL_GetTicks() + kAddSearchDebounceMs;
  }
  DirtyAddContact();
}

void ContactsController::RunAddSearch() {
  const std::string query = util::Trim(add_query_.c_str());
  if (query.empty()) {
    return;
  }
  if (!contacts_ports_.search_people) {
    add_searching_ = false;
    add_no_results_ = true;
    DirtyAddContact();
    return;
  }
  const uint64_t generation = add_search_generation_;
  AppRuntime::PostWorkerAndReplyOnUI<Roe<std::vector<mesh::DirectoryHit>>>(
      WorkerLane::Normal, [search = contacts_ports_.search_people, query]() { return search(query); },
      [query, generation](Roe<std::vector<mesh::DirectoryHit>> hits) {
        ContactsController::Instance().OnAddSearched(query, generation, std::move(hits));
      });
}

void ContactsController::OnAddSearched(const std::string& /*query*/, const uint64_t generation,
                                       Roe<std::vector<mesh::DirectoryHit>> hits) {
  if (!add_open_ || generation != add_search_generation_) {
    return; // typed on, or the sheet closed
  }
  add_searching_ = false;
  if (!hits) {
    if (ClassifyPeopleLookupError(hits.error()) == PeopleLookupFailure::DirectoryUnreachable) {
      UserFeedback::Fail(Tr("contacts.err.directory_unreachable"));
    } else {
      log().warning << "People lookup failed: " << AppError::Log(hits.error());
      UserFeedback::Fail(Tr("contacts.err.add"));
    }
    DirtyAddContact();
    return;
  }
  add_no_results_ = hits->empty();
  for (size_t i = 0; i < hits->size() && i < kAddMaxHits; ++i) {
    const mesh::DirectoryHit& hit = (*hits)[i];
    const std::string name = !hit.display_name.empty()
                                 ? hit.display_name
                                 : (!hit.nickname.empty() ? hit.nickname : Tr("people.result.unknown_person"));
    const std::string id = PrimaryAccountIdFromHit(hit).value_or(PrimaryRelayIdFromHit(hit).value_or(""));
    AddHitRow row;
    row.index = std::to_string(add_hits_.size()).c_str();
    row.title = name.c_str();
    row.short_id = ShortRelayId(id).c_str();
    const AvatarGlyph glyph = MakeAvatarGlyph(name, {});
    row.avatar_letter = glyph.letter.c_str();
    row.avatar_tone = glyph.tone;
    add_rows_.push_back(std::move(row));
    add_hits_.push_back(hit);
  }
  DirtyAddContact();
}

// A result chosen: the sheet goes, and the existing add flow (already a contact / ask first / add) takes over.
void ContactsController::OnAddPick(const std::string& index) {
  size_t i = 0;
  try {
    i = static_cast<size_t>(std::stoul(index));
  } catch (...) {
    return;
  }
  if (i >= add_hits_.size()) {
    return;
  }
  const mesh::DirectoryHit hit = add_hits_[i];
  const std::string query = util::Trim(add_query_.c_str());
  CloseAddContactPage();
  OfferHit(hit, query);
}

void ContactsController::OnAddAction(const std::string& action) {
  if (action == "scan") {
    CloseAddContactPage();
    OnScanQr();
  } else if (action == "qr") {
    if (!my_has_account_ || my_locator_.empty()) {
      ShowToast(Tr("settings.profile.no_peer_id"));
      return;
    }
    CloseAddContactPage();
    OpenMyQrPage();
  } else if (action == "invite") {
    OnAddShare({});
  } else if (action == "group") {
    CloseAddContactPage();
    OnNewGroup();
  }
}

void ContactsController::OnAddCopyId() {
  if (!my_has_account_) {
    return;
  }
  if (ui::SystemInterface* system = ui::GetSystemInterface()) {
    system->SetClipboardText(my_account_id_.c_str());
  }
  ShowToast(Tr("contacts.add_sheet.id_copied"));
}

void ContactsController::OnAddShare(const ShareAnchor anchor) {
  // Same invite as Me > Profile > Share: name, account id and the locator a friend pastes into Add contact.
  if (!my_has_account_) {
    ShowToast(Tr("settings.profile.no_peer_id"));
    return;
  }
  ShareInviteOrCopy(
      AccountInviteText(my_nickname_, my_account_id_.c_str(), my_locator_),
      [](const std::string& text, const ShareAnchor at) { return PlatformShareText(Backend::GetWindow(), text, at); },
      anchor, [](const std::string& text) {
        if (ui::SystemInterface* system = ui::GetSystemInterface()) {
          system->SetClipboardText(text.c_str());
        }
      });
}

void ContactsController::OpenMyQr() {
  LoadMyAccount();  // the scanner may open from outside the Add contact sheet, which loads it otherwise
  if (!my_has_account_ || my_locator_.empty()) {
    ShowToast(Tr("settings.profile.no_peer_id"));
    return;
  }
  OpenMyQrPage();
}

// My QR code: the invite locator as a QR in its own sheet (the Me > Profile "QR code" button shows it inline).
void ContactsController::OpenMyQrPage() {
  if (my_qr_open_ || !shell_navigation_.push_layer) {
    return;
  }
  auto qr = QrSvgContent(my_locator_);
  my_qr_svg_rml_ = qr ? qr.value().c_str() : "";
  DataModelHost::Instance().Dirty("contacts", "my_qr_svg_rml");
  my_qr_open_ = true;
  PaneSpec spec;
  spec.key = "my_qr";
  spec.page_sheet = true;
  my_qr_layer_id_ = shell_navigation_.push_layer(spec);
  if (flow_coordinator_.begin_modal) {
    flow_coordinator_.begin_modal(my_qr_layer_id_, {}, [this]() { OnMyQrDismissed(); });
  }
}

void ContactsController::OnMyQrDismissed() {
  my_qr_open_ = false;
  my_qr_layer_id_ = -1;
}

} // namespace pbr
