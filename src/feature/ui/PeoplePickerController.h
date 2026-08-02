#pragma once

#include "common/Module.h"
#include "feature/ui/ChatSessionPorts.h"
#include "feature/ui/PeoplePickerLogic.h"

#include <RmlUi/Core/DataModelHandle.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/Types.h>

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Rml {
class Context;
}

namespace pbr {

class MessagingHub;
class ProfileUnlockGate;
class FlowCoordinator;

class CallController;

class PeoplePickerController : public Module {
public:
  static PeoplePickerController& Instance();

  void BindMessaging(MessagingHub& messaging);
  void BindUnlockGate(ProfileUnlockGate& unlock_gate);
  void BindChatPorts(ChatSessionPorts ports);
  void BindFlowCoordinator(FlowCoordinator& flow);
  void BindCallController(CallController& call);
  MessagingHub& Hub();
  const MessagingHub& Hub() const;

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

  /** Group chat: pick members to invite to a new call. */
  void OpenForGroupCall(const std::string& thread_id, bool video);

  /** Active call: invite additional contacts as guests. */
  void OpenForCallAddGuest(const std::string& call_id);

  void Close();

private:
  PeoplePickerController();

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
  MessagingHub* messaging_ = nullptr;
  ProfileUnlockGate* unlock_gate_ = nullptr;
  FlowCoordinator* flow_ = nullptr;
  CallController* call_ = nullptr;
  ChatSessionPorts chat_ports_;
  std::string call_thread_id_;
  std::string call_id_;
  bool call_video_ = false;
  std::unordered_map<std::string, std::string> identity_for_contact_;

};

} // namespace pbr
