#pragma once

#include "common/Module.h"
#include "feature/conversations/MessagingContactsPorts.h"
#include "gui/ChatSessionPorts.h"
#include "gui/FlowCoordinatorPorts.h"
#include "gui/contacts/ContactsSurfaceNotifyPorts.h"
#include "gui/shell/ShellFeedbackPorts.h"
#include "gui/shell/ShellNavigationPorts.h"
#include "gui/UnlockEnsurePorts.h"
#include "gui/scan/ScanPorts.h"
#include "lib/ui/ContextMenuHost.h"

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
  /** The New chat sheet tells the flow coordinator it is open, so a scrim tap, X or swipe-down reaches it. */
  void BindFlowCoordinator(FlowCoordinatorPorts ports);
  /** Push surface snapshot to composition-root bridge. Clear via BindSurfaceNotify({}). */
  void BindSurfaceNotify(ContactsSurfaceNotifyPorts ports);
  void BindScanPorts(ScanPorts ports);

  struct ContactListRow {
    ui::String id;
    ui::String title;
    ui::String subtitle;
    /** Last 6 characters of the account id, for the phone row (never the whole id). */
    ui::String short_id;
    ui::String trust;
    int unread_count = 0;
    ui::String unread_display;
    bool has_icon = false;
    ui::String icon_src;
    ui::String avatar_letter = "?";
    int avatar_tone = 0;
  };

  /** One lettered group of a contact list: the phone "New chat" page, the Contacts tab. */
  struct NewChatSectionRow {
    ui::String letter;
    std::vector<ContactListRow> rows;
  };

  /** One letter of the phone Contacts index strip (A-Z, #); `present` when a section has that letter. */
  struct IndexLetterRow {
    ui::String letter;
    bool present = false;
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
    /** Last 6 characters of the account id, for the phone detail header. */
    ui::String short_id;
    ui::String display_name;
    ui::String nickname;
    /** Placeholder of the remark field. */
    ui::String name_placeholder;
    /** "Their nickname: X" under the remark field; empty when the contact has no nickname. */
    ui::String their_nickname;
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
    /** A group member who is not a contact (opened from a group chat): only name, photo, Account ID and Add. */
    bool member_preview = false;
    /** That member's account id (what Add contact looks up). */
    ui::String member_account;
  };

  bool RegisterModel(ui::Context* context);
  void OnNavTabActivated();
  void SyncLayoutMode();
  /** Reload list from store (e.g. after AI add_contact while tab is open). */
  void Refresh();
  /** Me → About → "Show advanced settings": ids, addresses and fingerprints in the contact detail. */
  void SetShowAdvanced(bool advanced);
  void Tick();
  void FlushPending();
  void OnSelectContact(const std::string& contact_id);
  /** A group member who is not a contact: their page in its member state, pushed like a contact's page. */
  void OnShowMember(const std::string& account_id, const std::string& name, const std::string& icon_src);
  void OnDetailDismissed();

  /** Phone Chats "compose": pushes the full-screen New chat page (AI chat / group / add contact / contacts). */
  void OpenNewChatPage();
  /** Phones (Settings > Security): the people this device blocked, each with Unblock; a tap opens their page. */
  void OpenBlockedPeople();
  /** The shell popped the blocked-people page. */
  void OnBlockedPeopleDismissed();
  /** How many saved contacts are blocked (the Settings row's count). */
  int BlockedCount() const;
  /** The New chat page left the screen (back, swipe, or a row was chosen). */
  void OnNewChatDismissed();

  /** Contacts → find people; also the first-run wizard's "Find friends". */
  /**
   * An invite link the OS opened the app with (a scanned QR code, a tapped link): once unlocked, asks
   * whether to add the person it names, then adds them as a pasted invite would. Anything else is ignored.
   */
  void OfferInvite(const std::string& link);
  void OnFindSomeone();
  /** Contacts → Scan QR code. Successful friend scans still ask before saving. */
  void OnScanQr();
  /**
   * Share a contact into a chat: `share` sends the card. When it fails and `refresh_card` is set, the contact's
   * card is asked for once (its devices, then the directory — the path opening the contact uses) and, if one
   * arrives, `share` runs once more. `done(shared)` exactly once. Exposed for tests.
   */
  static void ShareContactCard(const std::function<Roe<void>()>& share,
                               const std::function<void(std::function<void(bool kept)>)>& refresh_card,
                               std::function<void(bool shared)> done);
  /** The "+" menu: add contact always; on the phone also scan and new group (when wired), then find someone. Exposed for tests. */
  static std::vector<ContextMenuAction> AddContactMenuActions(bool compact, bool can_scan, bool can_new_group);
  /** The Contacts tab's rows in index order (A-Z by name, Chinese by pinyin initial, then #), one section per letter. */
  static std::vector<NewChatSectionRow> ContactSections(std::vector<ContactListRow> rows);
  /** The index strip: every letter A-Z then #, marked present where `sections` has it. */
  static std::vector<IndexLetterRow> ContactIndexLetters(const std::vector<NewChatSectionRow>& sections);
  /** The section an index letter jumps to: that letter's, else the next one after it, else the last; -1 if none. */
  static int ContactSectionForLetter(const std::vector<NewChatSectionRow>& sections, char letter);

private:
  void OnNewGroup();
  static void SelectContactCallback(ui::DataModelHandle model, ui::Event& ev, const ui::VariantList& args);
  static void BackToListCallback(ui::DataModelHandle model, ui::Event& ev, const ui::VariantList& args);
  static void StartChatCallback(ui::DataModelHandle model, ui::Event& ev, const ui::VariantList& args);
  static void SecureMessageCallback(ui::DataModelHandle model, ui::Event& ev, const ui::VariantList& args);
  static void AddMemberContactCallback(ui::DataModelHandle model, ui::Event& ev, const ui::VariantList& args);
  static void AddContactMenuCallback(ui::DataModelHandle model, ui::Event& ev, const ui::VariantList& args);
  static void FindSomeoneCallback(ui::DataModelHandle model, ui::Event& ev, const ui::VariantList& args);
  static void CopyIdCallback(ui::DataModelHandle model, ui::Event& ev, const ui::VariantList& args);
  static void ShareContactCallback(ui::DataModelHandle model, ui::Event& ev, const ui::VariantList& args);
  static void ToggleTrustFriendlyCallback(ui::DataModelHandle model, ui::Event& ev, const ui::VariantList& args);
  static void BlockContactCallback(ui::DataModelHandle model, ui::Event& ev, const ui::VariantList& args);
  static void UnblockContactCallback(ui::DataModelHandle model, ui::Event& ev, const ui::VariantList& args);
  static void RemoveContactCallback(ui::DataModelHandle model, ui::Event& ev, const ui::VariantList& args);
  static void OpenThreadCallback(ui::DataModelHandle model, ui::Event& ev, const ui::VariantList& args);
  static void OnSearchChangedCallback(ui::DataModelHandle model, ui::Event& ev, const ui::VariantList& args);
  static void OnContactFieldChangedCallback(ui::DataModelHandle model, ui::Event& ev, const ui::VariantList& args);
  static void NewChatPickCallback(ui::DataModelHandle model, ui::Event& ev, const ui::VariantList& args);
  static void NewChatActionCallback(ui::DataModelHandle model, ui::Event& ev, const ui::VariantList& args);
  static void NewChatSearchCallback(ui::DataModelHandle model, ui::Event& ev, const ui::VariantList& args);
  static void SyncRemoteCallback(ui::DataModelHandle model, ui::Event& ev, const ui::VariantList& args);

  void SyncFromStore();
  /** Rebuilds the New chat page's frequent and lettered rows from the contacts and chats (and its search). */
  void SyncNewChat();
  void DirtyNewChat();
  /** Closes the New chat sheet when it is open. */
  void CloseNewChatPage();
  void OnNewChatPick(const std::string& contact_id);
  void OnNewChatAction(const std::string& action);
  void LoadSelectedDetail(const std::string& contact_id);
  void OpenContactDetailPane();
  /** The selected contact cannot be messaged yet: ask for its account's card again (#409). */
  void MaybeRefreshContactCard(const std::string& contact_id);
  bool CloseContactDetailPane();
  void OnBackToList();
  void OnStartChat();
  void OnSecureMessage();
  void OnAddContactMenu(ui::Event& ev);
  /** A press or drag on the phone index strip: jump the list to the letter under the finger, show it in the bubble. */
  void OnContactsIndexAt(ui::Event& ev);
  /** The finger left the index strip: hide the bubble. */
  void OnContactsIndexEnd();
  void OnAddContact();
  void AddContactByQuery(const std::string& query);
  void OnPeopleSearched(const std::string& query, Roe<std::vector<mesh::DirectoryHit>> hits);
  void AddContactFromHit(const mesh::DirectoryHit& hit);
  std::optional<Contact> FindContactByAccount(const std::string& account_id) const;
  void ShowAlreadyContact(const Contact& contact);
  void OnCopyId();
  void OnShareContact(ui::Vector2i position);
  void ShareContactTo(const std::string& thread_id, const std::string& contact_id, const std::string& name);
  /** "Add contact" with a pasted invite: the contact added, shown (UI thread). */
  void OnAddedFromInvite(const Roe<std::pair<std::string, bool>>& added);
  void OnSetTrust(const std::string& trust);
  /** Saves `trust` for the contact; false (and a failure shown) when it could not be saved. */
  bool SetContactTrust(const std::string& contact_id, const std::string& trust);
  void SyncBlocked();
  void DirtyBlocked();
  void OnUnblockListed(const std::string& contact_id);
  void OnOpenBlockedContact(const std::string& contact_id);
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
  std::vector<NewChatSectionRow> contact_sections_;
  std::vector<IndexLetterRow> contact_index_;
  /** The letter in the centred bubble while a finger is on the index strip; empty otherwise. */
  ui::String contact_index_bubble_;
  std::vector<ContactListRow> new_chat_frequent_;
  std::vector<NewChatSectionRow> new_chat_sections_;
  ui::String new_chat_query_;
  bool new_chat_open_ = false;
  /** Blocked people, by name (OpenBlockedPeople). */
  std::vector<ContactListRow> blocked_rows_;
  bool blocked_open_ = false;
  /** The New chat sheet's overlay layer (-1: not open). */
  int new_chat_layer_id_ = -1;
  ui::String search_query_;
  bool compact_layout_ = false;
  bool advanced_ = false;
  ContactDetail selected_;
  ui::Context* context_ = nullptr;
  bool contact_dirty_ = false;
  uint64_t debounce_deadline_ms_ = 0;
  MessagingContactsPorts contacts_ports_;
  UnlockEnsurePorts unlock_ensure_;
  ChatSessionPorts chat_ports_;
  ShellNavigationPorts shell_navigation_;
  ShellFeedbackPorts shell_feedback_;
  FlowCoordinatorPorts flow_coordinator_;
  ContactsSurfaceNotifyPorts surface_notify_;
  ScanPorts scan_ports_;

  static ContactsController* installed_instance_;
};

/** Detail form for a contact (title, remark, their nickname, ...). Exposed for tests. */
ContactsController::ContactDetail ToContactDetail(const Contact& contact, const MessagingContactsPorts& ports);
/** The page of a group member who is not a contact: name (else "Unnamed"), photo, last six of the account id. */
ContactsController::ContactDetail MemberPreviewDetail(const std::string& account_id, const std::string& name,
                                                      const std::string& icon_src);
/** Apply the editable detail fields (the remark, capped at kMaxNicknameChars) to a copy of the contact. */
Contact BuildContactFromDetail(const Contact& existing, const ContactsController::ContactDetail& detail);

/** The blocked ones of `contacts`, sorted by name as the Contacts list sorts them. */
std::vector<Contact> BlockedContactsByName(const std::vector<Contact>& contacts);

} // namespace pbr
