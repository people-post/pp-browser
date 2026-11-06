#pragma once

#include "common/Module.h"
#include "feature/presenters/ChatPresenter.h"
#include "feature/conversations/MessagingContactsPorts.h"
#include "feature/product/MessagingPeoplePickerPorts.h"
#include "gui/CallActionsPorts.h"
#include "gui/ChatSessionPorts.h"
#include "gui/contacts/ContactsNotifyPorts.h"
#include "gui/FlowCoordinatorPorts.h"
#include "lib/ui/PeoplePickerLogic.h"
#include "gui/contacts/PeoplePickerSurfaceNotifyPorts.h"
#include "gui/shell/ShellFeedbackPorts.h"
#include "gui/shell/ShellNavigationPorts.h"
#include "gui/UnlockEnsurePorts.h"

#include <ui/data/DataModelHandle.h>
#include <ui/dom/Event.h>
#include <ui/base/Types.h>
#include <ui/base/Vector2.h>

#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "common/PbrCompat.h"

namespace ui {
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
  /** Shell navigation / layers. Clear via BindShellNavigation({}). */
  void BindShellNavigation(ShellNavigationPorts ports);
  /** Toast feedback. Clear via BindShellFeedback({}). */
  void BindShellFeedback(ShellFeedbackPorts ports);
  /** Push surface snapshot to app PeoplePickerShellBridge. */
  void BindSurfaceNotify(PeoplePickerSurfaceNotifyPorts ports);
  /** A member's profile (a page sheet on phones). Clear via BindContactsNotify({}). */
  void BindContactsNotify(ContactsNotifyPorts ports);

  struct PickerRow {
    ui::String id;
    ui::String title;
    ui::String subtitle;
    bool selected = false;
    bool locked = false;
    /** Greyed out and not pickable (the group is full). */
    bool disabled = false;
  };

  struct MemberSummaryRow {
    ui::String title;
  };

  bool RegisterModel(ui::Context* context);

  /** Sessions + / Message a contact: 1→DM, 2+→group. */
  void OpenFree();

  /** From open DM: peer locked; create group when ≥1 extra selected. */
  void OpenFromDm(const std::string& locked_contact_id);

  /** OpenForGroupCall: optional Allow video checkbox, pre-checked to the caller's menu choice. */
  void OpenForGroupCall(const std::string& thread_id, bool video_allowed);

  /** Active call: invite additional contacts as guests. */
  void OpenForCallAddGuest(const std::string& call_id);

  /**
   * G022: "Make a group…" in the call `call_id`: everyone in it picked (each can be unticked), Next names the group
   * (a default from their names), Create makes it with us as owner and invites them.
   */
  void OpenForCallMakeGroup(const std::string& call_id);

  /** The member list of the group chat `thread_id`; the owner adds, removes and re-invites from it. */
  void OpenForGroupMembers(const std::string& thread_id);

  /** Add members to the group chat `thread_id` straight away (the group info sheet's "Add members"); closes when done. */
  void OpenForGroupAddMembers(const std::string& thread_id);

  /** Someone tapped on the group info sheet's member card: the same as a tap in the member list. */
  void TapGroupMember(const std::string& thread_id, const std::string& identity);
  /**
   * A member's profile was just opened from the group chat `thread_id` (their avatar or name; `sender` a contact id or
   * an account id): if we own the group, the profile gets the "Group admin" card.
   */
  void OfferGroupAdmin(const std::string& thread_id, const std::string& sender);

  void Close();

private:
  static void ToggleContactCallback(ui::DataModelHandle model, ui::Event& ev, const ui::VariantList& args);
  static void OnSearchChangedCallback(ui::DataModelHandle model, ui::Event& ev, const ui::VariantList& args);
  static void ConfirmCallback(ui::DataModelHandle model, ui::Event& ev, const ui::VariantList& args);
  static void CancelCallback(ui::DataModelHandle model, ui::Event& ev, const ui::VariantList& args);
  static void BackCallback(ui::DataModelHandle model, ui::Event& ev, const ui::VariantList& args);
  static void CreateGroupCallback(ui::DataModelHandle model, ui::Event& ev, const ui::VariantList& args);
  static void AddMembersCallback(ui::DataModelHandle model, ui::Event& ev, const ui::VariantList& args);

  void Open(PeoplePickerMode mode, std::unordered_set<std::string> locked_ids);
  void SyncRows();
  void SyncGroupCallRows();
  void SyncCallAddGuestRows();
  /** G022: the call's people but us, as account rows (they need not be contacts). */
  void SyncCallMakeGroupRows();
  /** Someone in a call by their account: their contact's name, the nickname on their card, else unnamed. */
  std::string CallMemberName(const std::string& identity) const;
  /** The name step's default for `ids` (accounts in CallMakeGroup; else the usual default). */
  std::string DefaultGroupTitleFor(const std::vector<std::string>& ids) const;
  /** The select step's title for the mode open. */
  std::string SelectStepTitle() const;
  void SyncGroupMemberRows();
  void SyncGroupAddRows();
  void OpenForGroupAddMember();
  void RefreshGroupAddLimit();
  std::optional<Contact> ContactForMember(const GroupRosterMember& member) const;
  void OnMemberTapped(const std::string& identity, ui::Vector2i menu_position);
  /** Phones: everyone goes to the profile; the owner's has the "Group admin" card (DecideGroupMemberTap). */
  void ShowMemberTap(const GroupMemberRow& row, const std::string& group_id, MemberRole viewer_role, bool is_self);
  /** The owner's card on the profile just opened for `row` (nothing when `tap` offers no admin action). */
  void OfferAdminCard(const GroupMemberTap& tap, const std::string& group_id, const GroupMemberRow& row);
  /** Someone on the roster of the group chat `thread_id`, found by `matches`, with what we know about them and us. */
  struct MemberInGroup {
    std::string group_id;
    GroupMemberRow row;
    /** Ours in the group: Owner when its recorded owner is us, else the roster's (an admin removes members). */
    MemberRole viewer_role = MemberRole::Member;
    bool is_self = false;
  };
  std::optional<MemberInGroup> FindMemberInGroup(const std::string& thread_id,
                                                 const std::function<bool(const GroupRosterMember&)>& matches) const;
  /** The member's profile as a page sheet (their contact page, or its member state). */
  void OpenMemberProfile(const GroupMemberRow& row);
  bool CompactLayout() const;
  void ReinviteMember(const std::string& group_id, const GroupMemberRow& row);
  /** Asks first; `removed` runs once the person is out (the owner's profile sheet then closes). */
  void RemoveMember(const std::string& group_id, const GroupMemberRow& row, std::function<void()> removed = {});
  /** Owner (G017): make `row` an admin, or a member again; a toast says which, the rows refresh. */
  void SetMemberAdmin(const std::string& group_id, const GroupMemberRow& row, bool admin);
  void AddMemberAsContact(const GroupMemberRow& row);
  void OnInviteMembers();
  void OnInviteFinished(uint64_t batch, const std::string& group_id, const std::unordered_map<std::string, std::string>& names,
                        const GroupInviteBatchResult& result);
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
  /** Closes Add members and returns to the member list it was opened from. */
  void CloseAddMembers();
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

  ui::Context* context_ = nullptr;
  int layer_id_ = -1;
  PeoplePickerMode mode_ = PeoplePickerMode::Free;
  std::unordered_set<std::string> locked_ids_;
  std::unordered_set<std::string> selected_ids_;
  std::vector<std::string> pending_member_ids_;
  std::vector<PickerRow> rows_;
  std::vector<MemberSummaryRow> member_summary_;
  ui::String search_query_;
  ui::String title_;
  ui::String step_;
  ui::String group_title_;
  ui::String group_title_help_;
  ui::String cta_label_;
  ui::String empty_hint_;
  bool cta_enabled_ = false;
  bool show_checks_ = true;
  bool show_cta_ = true;
  bool show_add_members_ = false;
  ui::String full_hint_;
  ui::String invite_hint_;
  MessagingContactsPorts contacts_ports_;
  MessagingPeoplePickerPorts picker_ports_;
  UnlockEnsurePorts unlock_ensure_;
  FlowCoordinatorPorts flow_coordinator_;
  CallActionsPorts call_actions_;
  ChatSessionPorts chat_ports_;
  ShellNavigationPorts shell_navigation_;
  ShellFeedbackPorts shell_feedback_;
  PeoplePickerSurfaceNotifyPorts surface_notify_;
  ContactsNotifyPorts contacts_notify_;
  std::string call_thread_id_;
  std::string call_id_;
  bool call_video_allowed_ = false;
  bool show_call_video_option_ = false;
  std::unordered_map<std::string, std::string> identity_for_contact_;
  // GroupMembers / GroupAddMember
  std::string group_id_;
  std::string group_thread_id_;
  bool group_is_owner_ = false;
  /** Our role in the group listed (Owner when we are its recorded owner). */
  MemberRole group_viewer_role_ = MemberRole::Member;
  /** This account, on the member list (its own row is not tappable). */
  std::string local_identity_;
  bool group_sending_ = false;
  /** The latest invite batch; an older one finishing does not end the current send. */
  uint64_t invite_batch_ = 0;
  int group_roster_count_ = 0;
  /** Add members was opened from the member list (its Cancel / Invite go back there), not over the group info. */
  bool add_returns_to_list_ = true;
  std::unordered_map<std::string, GroupMemberRow> member_rows_;

  static PeoplePickerController* installed_instance_;
};

} // namespace pbr
