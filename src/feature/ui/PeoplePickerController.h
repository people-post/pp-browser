#pragma once

#include "common/Module.h"
#include "feature/messaging/MessagingContactsPorts.h"
#include "feature/messaging/MessagingPeoplePickerPorts.h"
#include "feature/ui/CallActionsPorts.h"
#include "feature/ui/ChatSessionPorts.h"
#include "feature/ui/FlowCoordinatorPorts.h"
#include "feature/ui/PeoplePickerLogic.h"
#include "feature/ui/PeoplePickerSurfaceNotifyPorts.h"
#include "feature/ui/ShellFeedbackPorts.h"
#include "feature/ui/ShellNavigationPorts.h"
#include "feature/ui/UnlockEnsurePorts.h"

#include <RmlUi/Core/DataModelHandle.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/Types.h>

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "common/PbrCompat.h"

namespace Rml {
class Context;
}

namespace pbr {

class PeoplePickerController : public Module {
public:
  PeoplePickerController();
  ~PeoplePickerController() override = default;

  /** App-owned instance; set via InstallInstance from Application. Static callbacks use Instance(). */
  static void InstallInstance(PeoplePickerController& controller);
  static void ClearInstance();
  static PeoplePickerController& Instance();

  void BindContactsPorts(MessagingContactsPorts ports);
  void BindPickerPorts(MessagingPeoplePickerPorts ports);
  void BindUnlockEnsure(UnlockEnsurePorts ports);
  void BindChatPorts(ChatSessionPorts ports);
  void BindFlowCoordinator(FlowCoordinatorPorts ports);
  void BindCallActions(CallActionsPorts ports);
  /** Shell navigation / layers without ShellHost::Instance(). Clear via BindShellNavigation({}). */
  void BindShellNavigation(ShellNavigationPorts ports);
  /** Toast feedback without ShellHost::Instance(). Clear via BindShellFeedback({}). */
  void BindShellFeedback(ShellFeedbackPorts ports);
  /** Push surface snapshot to app PeoplePickerShellBridge. */
  void BindSurfaceNotify(PeoplePickerSurfaceNotifyPorts ports);

  struct PickerRow {
    Rml::String id;
    Rml::String title;
    Rml::String subtitle;
    bool selected = false;
    bool locked = false;
  };

  struct MemberSummaryRow {
    Rml::String title;
  };

  bool RegisterModel(Rml::Context* context);

  /** Sessions + / Message a contact: 1→DM, 2+→group. */
  void OpenFree();

  /** From open DM: peer locked; create group when ≥1 extra selected. */
  void OpenFromDm(const std::string& locked_contact_id);

  /** OpenForGroupCall: optional Allow video checkbox (default off). */
  void OpenForGroupCall(const std::string& thread_id);

  /** Active call: invite additional contacts as guests. */
  void OpenForCallAddGuest(const std::string& call_id);

  void Close();

private:
  static void ToggleContactCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void OnSearchChangedCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void ConfirmCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void CancelCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void BackCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void CreateGroupCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);

  void Open(PeoplePickerMode mode, std::unordered_set<std::string> locked_ids);
  void SyncRows();
  void SyncGroupCallRows();
  void SyncCallAddGuestRows();
  void UpdateCta();
  void DirtyAll();
  void OnToggleContact(const std::string& contact_id);
  void OnSearchChanged();
  void OnConfirm();
  void OnCancel();
  void OnBack();
  void OnCreateGroup();
  void OnStartCall();
  void AdvanceToNameStep(const std::vector<std::string>& member_contact_ids);
  void GoBackToSelect();
  void FinishOpenThread();
  void StartDirectMessage(const std::string& contact_id);
  void CreateGroupWithTitle(const std::vector<std::string>& member_contact_ids, std::string title);
  void ResetState();
  void OnFlowDismissed();
  void RegisterFlow();
  std::vector<std::string> SelectedContactIds() const;
  std::vector<std::string> SelectedInviteIdentities() const;
  int FreeSelectedCount() const;
  std::string TitleForContactId(const std::string& contact_id) const;
  std::string TrimTitle(std::string title) const;
  PeoplePickerSurfaceSnapshot BuildSurfaceSnapshot() const;
  void NotifySurfaceChanged();
  void ShowToast(const std::string& message, ToastDuration duration = ToastDuration::Short);
  bool MessagingInitialized() const;
  bool MessagingReady() const;

  Rml::Context* context_ = nullptr;
  int layer_id_ = -1;
  PeoplePickerMode mode_ = PeoplePickerMode::Free;
  std::unordered_set<std::string> locked_ids_;
  std::unordered_set<std::string> selected_ids_;
  std::vector<std::string> pending_member_ids_;
  std::vector<PickerRow> rows_;
  std::vector<MemberSummaryRow> member_summary_;
  Rml::String search_query_;
  Rml::String title_;
  Rml::String step_;
  Rml::String group_title_;
  Rml::String group_title_help_;
  Rml::String cta_label_;
  Rml::String empty_hint_;
  bool cta_enabled_ = false;
  MessagingContactsPorts contacts_ports_;
  MessagingPeoplePickerPorts picker_ports_;
  UnlockEnsurePorts unlock_ensure_;
  FlowCoordinatorPorts flow_coordinator_;
  CallActionsPorts call_actions_;
  ChatSessionPorts chat_ports_;
  ShellNavigationPorts shell_navigation_;
  ShellFeedbackPorts shell_feedback_;
  PeoplePickerSurfaceNotifyPorts surface_notify_;
  std::string call_thread_id_;
  std::string call_id_;
  bool call_video_allowed_ = false;
  bool show_call_video_option_ = false;
  std::unordered_map<std::string, std::string> identity_for_contact_;

  static PeoplePickerController* installed_instance_;
};

} // namespace pbr
