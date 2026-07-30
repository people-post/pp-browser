#pragma once

#include "common/Module.h"
#include "feature/ui/ChatSessionPorts.h"

#include <RmlUi/Core/DataModelHandle.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/Types.h>

#include <cstdint>
#include <vector>

namespace Rml {
class Context;
}

namespace pbr {

class MessagingHub;

struct Contact;

class ContactsController : public Module {
public:
  static ContactsController& Instance();

  void BindMessaging(MessagingHub& messaging);
  void BindChatPorts(ChatSessionPorts ports);
  MessagingHub& Hub();
  const MessagingHub& Hub() const;

  struct ContactListRow {
    Rml::String id;
    Rml::String title;
    Rml::String subtitle;
    Rml::String trust;
    int unread_count = 0;
    Rml::String unread_display;
  };

  struct ContactIdentityRow {
    Rml::String label;
    Rml::String value;
    Rml::String kind;
    bool is_primary = false;
  };

  struct ContactThreadRow {
    Rml::String id;
    Rml::String title;
    Rml::String channel_label;
    Rml::String kind;
    int unread_count = 0;
    Rml::String unread_display;
  };

  struct ContactDetail {
    Rml::String id;
    Rml::String display_name;
    Rml::String nickname;
    Rml::String relay_id;
    Rml::String peer_id;
    Rml::String multiaddrs_text;
    Rml::String trust;
    Rml::String trust_key;
    Rml::String signing_fingerprint;
    Rml::String message_hint;
    std::vector<ContactIdentityRow> identities;
    std::vector<ContactThreadRow> threads;
    bool can_message = false;
  };

  bool RegisterModel(Rml::Context* context);
  void OnNavTabActivated();
  void SyncLayoutMode();
  /** Reload list from store (e.g. after AI add_contact while tab is open). */
  void Refresh();
  void Tick();
  void FlushPending();
  void OnSelectContact(const std::string& contact_id);
  void OnDetailDismissed();

private:
  ContactsController();

  static void SelectContactCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void BackToListCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void StartChatCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void SecureMessageCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void AddContactMenuCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void FindSomeoneCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void CopyIdCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void ShareContactCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void SetTrustCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void RemoveContactCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void OpenThreadCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void OnSearchChangedCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void OnContactFieldChangedCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);

  void SyncFromStore();
  void LoadSelectedDetail(const std::string& contact_id);
  void OpenContactDetailPane();
  bool CloseContactDetailPane();
  void OnBackToList();
  void OnStartChat();
  void OnSecureMessage();
  void OnAddContactMenu(Rml::Event& ev);
  void OnAddContact();
  void OnFindSomeone();
  void OnCopyId();
  void OnShareContact();
  void OnSetTrust(const std::string& trust);
  void OnRemoveContact();
  void OnOpenThread(const std::string& thread_id);
  void OnSearchChanged();
  void OnContactFieldChanged();
  bool FlushSelectedContact();
  void UpdateMessagingEligibility(const Contact& contact);
  void DirtyAll();

  std::vector<ContactListRow> contacts_;
  Rml::String search_query_;
  bool compact_layout_ = false;
  ContactDetail selected_;
  Rml::Context* context_ = nullptr;
  bool contact_dirty_ = false;
  uint64_t debounce_deadline_ms_ = 0;
  MessagingHub* messaging_ = nullptr;
  ChatSessionPorts chat_ports_;

};

} // namespace pbr
