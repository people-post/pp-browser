#pragma once

#include "common/Module.h"
#include "feature/messaging/MessagingContactsPorts.h"
#include "feature/ui/ChatSessionPorts.h"
#include "feature/ui/ContactsSurfaceNotifyPorts.h"
#include "feature/ui/ShellFeedbackPorts.h"
#include "feature/ui/ShellNavigationPorts.h"
#include "feature/ui/UnlockEnsurePorts.h"

#include <RmlUi/Core/DataModelHandle.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/Types.h>

#include <cstdint>
#include <functional>
#include <vector>

namespace Rml {
class Context;
}

namespace pbr {

struct Contact;

class ContactsController : public Module {
public:
  ContactsController();
  ~ContactsController() override = default;

  /** App-owned instance; set via InstallInstance from Application. Static callbacks use Instance(). */
  static void InstallInstance(ContactsController& controller);
  static void ClearInstance();
  static ContactsController& Instance();

  void BindContactsPorts(MessagingContactsPorts ports);
  void BindUnlockEnsure(UnlockEnsurePorts ports);
  void BindChatPorts(ChatSessionPorts ports);
  void BindShellNavigation(ShellNavigationPorts ports);
  void BindShellFeedback(ShellFeedbackPorts ports);
  /** Push surface snapshot to composition-root bridge. Clear via BindSurfaceNotify({}). */
  void BindSurfaceNotify(ContactsSurfaceNotifyPorts ports);

  struct ContactListRow {
    Rml::String id;
    Rml::String title;
    Rml::String subtitle;
    Rml::String trust;
    int unread_count = 0;
    Rml::String unread_display;
    bool has_icon = false;
    Rml::String icon_src;
    Rml::String avatar_letter = "?";
    int avatar_tone = 0;
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
    bool has_icon = false;
    Rml::String icon_src;
    Rml::String avatar_letter = "?";
    int avatar_tone = 0;
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
  ContactsSurfaceSnapshot BuildSurfaceSnapshot() const;
  /** Push surface snapshot to app bridge (no shell chrome knowledge). */
  void NotifySurfaceChanged();
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
  MessagingContactsPorts contacts_ports_;
  UnlockEnsurePorts unlock_ensure_;
  ChatSessionPorts chat_ports_;
  ShellNavigationPorts shell_navigation_;
  ShellFeedbackPorts shell_feedback_;
  ContactsSurfaceNotifyPorts surface_notify_;

  static ContactsController* installed_instance_;
};

} // namespace pbr
