#pragma once

#include "common/Module.h"
#include "feature/ui/ChatSessionPorts.h"
#include "feature/ui/ShellFeedbackPorts.h"
#include "feature/ui/ShellNavigationPorts.h"

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
class ProfileUnlockGate;
struct Contact;

class ContactsController : public Module {
public:
  static ContactsController& Instance();

  void BindMessaging(MessagingHub& messaging);
  void BindUnlockGate(ProfileUnlockGate& unlock_gate);
  void BindChatPorts(ChatSessionPorts ports);
  void BindShellNavigation(ShellNavigationPorts ports);
  void BindShellFeedback(ShellFeedbackPorts ports);
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
    Rml::String title;
    Rml::String subtitle;
    Rml::String display_name;
    Rml::String nickname;
    Rml::String relay_id;
    Rml::String peer_id;
    Rml::String multiaddrs_text;
    Rml::String multiaddrs_summary;
    Rml::String trust;
    Rml::String trust_key;
    Rml::String signing_fingerprint;
    Rml::String message_hint;
    Rml::String remote_updated;
    std::vector<ContactIdentityRow> identities;
    std::vector<ContactThreadRow> threads;
    bool can_message = false;
    bool has_relay_id = false;
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
  static void SyncRemoteCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);

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
  void OnSyncRemote();
  bool FlushSelectedContact();
  void UpdateMessagingEligibility(const Contact& contact);
  void DirtyAll();

  ShellChromeSnapshot ChromeSnapshot() const;
  void ShellDirty();
  void ShellSyncLayout(bool restore_focus_after = false);
  void ShowToast(const std::string& message, ToastDuration duration = ToastDuration::Short);
  void ShowConfirm(const std::string& title, const std::string& message, std::function<void(bool)> on_result);
  void NavigateToChatSession();

  std::vector<ContactListRow> contacts_;
  Rml::String search_query_;
  bool compact_layout_ = false;
  ContactDetail selected_;
  Rml::Context* context_ = nullptr;
  bool contact_dirty_ = false;
  uint64_t debounce_deadline_ms_ = 0;
  MessagingHub* messaging_ = nullptr;
  ProfileUnlockGate* unlock_gate_ = nullptr;
  ChatSessionPorts chat_ports_;
  ShellNavigationPorts shell_navigation_;
  ShellFeedbackPorts shell_feedback_;

};

} // namespace pbr
