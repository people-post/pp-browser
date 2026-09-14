#pragma once

#include "common/Module.h"
#include "feature/conversations/MessagingContactsPorts.h"
#include "gui/ChatSessionPorts.h"
#include "gui/contacts/ContactsSurfaceNotifyPorts.h"
#include "gui/shell/ShellFeedbackPorts.h"
#include "gui/shell/ShellNavigationPorts.h"
#include "gui/UnlockEnsurePorts.h"

#include <ui/data/DataModelHandle.h>
#include <ui/dom/Event.h>
#include <ui/base/Types.h>

#include <cstdint>
#include <functional>
#include <vector>
#include "common/PbrCompat.h"

namespace ui {
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
    ui::String id;
    ui::String title;
    ui::String subtitle;
    ui::String trust;
    int unread_count = 0;
    ui::String unread_display;
    bool has_icon = false;
    ui::String icon_src;
    ui::String avatar_letter = "?";
    int avatar_tone = 0;
  };

  struct ContactIdentityRow {
    ui::String label;
    ui::String value;
    ui::String kind;
    bool is_primary = false;
  };

  struct ContactThreadRow {
    ui::String id;
    ui::String title;
    ui::String channel_label;
    ui::String kind;
    int unread_count = 0;
    ui::String unread_display;
  };

  struct ContactDetail {
    ui::String id;
    ui::String title;
    ui::String subtitle;
    ui::String display_name;
    ui::String nickname;
    ui::String relay_id;
    ui::String peer_id;
    ui::String multiaddrs_text;
    ui::String multiaddrs_summary;
    ui::String trust;
    ui::String trust_key;
    ui::String signing_fingerprint;
    ui::String message_hint;
    ui::String remote_updated;
    std::vector<ContactIdentityRow> identities;
    std::vector<ContactThreadRow> threads;
    bool can_message = false;
    bool has_relay_id = false;
    bool has_icon = false;
    ui::String icon_src;
    ui::String avatar_letter = "?";
    int avatar_tone = 0;
  };

  bool RegisterModel(ui::Context* context);
  void OnNavTabActivated();
  void SyncLayoutMode();
  /** Reload list from store (e.g. after AI add_contact while tab is open). */
  void Refresh();
  void Tick();
  void FlushPending();
  void OnSelectContact(const std::string& contact_id);
  void OnDetailDismissed();

private:
  static void SelectContactCallback(ui::DataModelHandle model, ui::Event& ev, const ui::VariantList& args);
  static void BackToListCallback(ui::DataModelHandle model, ui::Event& ev, const ui::VariantList& args);
  static void StartChatCallback(ui::DataModelHandle model, ui::Event& ev, const ui::VariantList& args);
  static void SecureMessageCallback(ui::DataModelHandle model, ui::Event& ev, const ui::VariantList& args);
  static void AddContactMenuCallback(ui::DataModelHandle model, ui::Event& ev, const ui::VariantList& args);
  static void FindSomeoneCallback(ui::DataModelHandle model, ui::Event& ev, const ui::VariantList& args);
  static void CopyIdCallback(ui::DataModelHandle model, ui::Event& ev, const ui::VariantList& args);
  static void ShareContactCallback(ui::DataModelHandle model, ui::Event& ev, const ui::VariantList& args);
  static void SetTrustCallback(ui::DataModelHandle model, ui::Event& ev, const ui::VariantList& args);
  static void RemoveContactCallback(ui::DataModelHandle model, ui::Event& ev, const ui::VariantList& args);
  static void OpenThreadCallback(ui::DataModelHandle model, ui::Event& ev, const ui::VariantList& args);
  static void OnSearchChangedCallback(ui::DataModelHandle model, ui::Event& ev, const ui::VariantList& args);
  static void OnContactFieldChangedCallback(ui::DataModelHandle model, ui::Event& ev, const ui::VariantList& args);
  static void SyncRemoteCallback(ui::DataModelHandle model, ui::Event& ev, const ui::VariantList& args);

  void SyncFromStore();
  void LoadSelectedDetail(const std::string& contact_id);
  void OpenContactDetailPane();
  bool CloseContactDetailPane();
  void OnBackToList();
  void OnStartChat();
  void OnSecureMessage();
  void OnAddContactMenu(ui::Event& ev);
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
  ui::String search_query_;
  bool compact_layout_ = false;
  ContactDetail selected_;
  ui::Context* context_ = nullptr;
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
