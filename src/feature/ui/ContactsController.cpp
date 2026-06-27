#include "feature/ui/ContactsController.h"

#include "base/messaging/MessagingJson.h"
#include "base/people/ContactTypes.h"
#include "feature/messaging/MessagingHub.h"
#include "feature/chat/ChatController.h"
#include "feature/ui/DataModelHost.h"
#include "feature/ui/ShellHost.h"

#include "base/ui/ShellTypes.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/DataModelHandle.h>
#include <RmlUi/Core/Event.h>

#include <algorithm>

namespace pbr {

namespace {

std::string PrimaryRelayId(const Contact& contact) {
  for (const ContactId& id : contact.ids) {
    if (id.kind == ContactIdKind::RelayUser && id.primary) {
      return id.value;
    }
  }
  for (const ContactId& id : contact.ids) {
    if (id.kind == ContactIdKind::RelayUser) {
      return id.value;
    }
  }
  return {};
}

ContactsController::ContactDetail ToContactDetail(const Contact& contact) {
  ContactsController::ContactDetail detail;
  detail.id = contact.id.c_str();
  detail.display_name = contact.display_name.c_str();
  detail.nickname = contact.server_nickname.c_str();
  detail.relay_id = PrimaryRelayId(contact).c_str();
  detail.trust = TrustLevelToString(contact.trust).c_str();
  return detail;
}

ContactsController::ContactListRow ToContactListRow(const Contact& contact) {
  ContactsController::ContactListRow row;
  row.id = contact.id.c_str();
  row.title = contact.display_name.empty() ? contact.server_nickname.c_str() : contact.display_name.c_str();
  if (!contact.server_nickname.empty() && contact.server_nickname != contact.display_name) {
    row.subtitle = contact.server_nickname.c_str();
  } else {
    row.subtitle = PrimaryRelayId(contact).c_str();
  }
  return row;
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
    }
    if (auto detail_handle = ctor.RegisterStruct<ContactDetail>()) {
      detail_handle.RegisterMember("id", &ContactDetail::id);
      detail_handle.RegisterMember("display_name", &ContactDetail::display_name);
      detail_handle.RegisterMember("nickname", &ContactDetail::nickname);
      detail_handle.RegisterMember("relay_id", &ContactDetail::relay_id);
      detail_handle.RegisterMember("trust", &ContactDetail::trust);
    }
    ctor.RegisterArray<std::vector<ContactListRow>>();
    ctor.Bind("contacts", &controller.contacts_);
    ctor.Bind("compact_layout", &controller.compact_layout_);
    ctor.Bind("show_detail", &controller.show_detail_);
    ctor.Bind("selected", &controller.selected_);
    ctor.BindEventCallback("select_contact", &ContactsController::SelectContactCallback);
    ctor.BindEventCallback("back_to_list", &ContactsController::BackToListCallback);
    ctor.BindEventCallback("start_chat", &ContactsController::StartChatCallback);
  });
}

void ContactsController::DirtyAll() {
  auto& host = DataModelHost::Instance();
  host.Dirty("contacts", "contacts");
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

  auto stored = MessagingHub::Instance().Contacts().List();
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

void ContactsController::OnNavTabActivated() {
  show_detail_ = false;
  selected_ = {};
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

void ContactsController::OnSelectContact(const std::string& contact_id) {
  if (!MessagingHub::Instance().IsInitialized()) {
    return;
  }

  auto contact = MessagingHub::Instance().Contacts().Get(contact_id);
  if (!contact || !*contact) {
    return;
  }

  selected_ = ToContactDetail(**contact);
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
  if (!MessagingHub::Instance().Inbox().FindOrCreateDirectThread(contact_id)) {
    return;
  }

  ShellHost::Instance().SelectNavTab(NavTab::Sessions);
  ShellHost::Instance().SetPrimaryPane("chat");
  ChatController::Instance().FinalizeThreadDisplay();
}

} // namespace pbr
