#pragma once

#include "common/Lifetime.h"
#include "common/Module.h"
#include "feature/conversations/MessagingContactsPorts.h"
#include "gui/CallActionsPorts.h"
#include "gui/ChatSessionPorts.h"
#include "gui/FlowCoordinatorPorts.h"
#include "gui/contacts/ContactsAccountPorts.h"
#include "gui/contacts/ContactsNotifyPorts.h"
#include "gui/contacts/ContactsSurfaceNotifyPorts.h"
#include "gui/shell/ShellFeedbackPorts.h"
#include "gui/shell/ShellNavigationPorts.h"
#include "gui/UnlockEnsurePorts.h"
#include "gui/scan/ScanPorts.h"
#include "lib/ui/ContextMenuHost.h"

#include <ui/data/DataModelHandle.h>
#include <ui/dom/Event.h>
#include <ui/base/Types.h>

#include "foundation/platform/PlatformShare.h"

#include <cstdint>
#include <functional>
#include <set>
#include <string>
#include <utility>
#include <vector>
#include "common/PbrCompat.h"

namespace ui {
class Context;
}

namespace pbr {

struct Contact;
class SessionStore;

/**
 * The pill after the remark auto-saved (Kenneth 2026-11-04: "保存成功……保存不成功，都给他个提示"): once per edit, not
 * per keystroke. A save that changed the remark is announced when the field is left or the page goes (Take); a failed
 * one at once, by the caller.
 */
class RemarkSaveNotice {
public:
  /** A save of the contact page ended; `remark_changed`: it changed the stored remark. */
  void OnSaved(bool remark_changed, bool ok) {
    if (remark_changed) {
      saved_ = ok;
    }
  }
  /** The field was left or the page goes: the key of the pill to show now ("" for none); only once per edit. */
  std::string Take() { return std::exchange(saved_, false) ? "contacts.remark.saved" : std::string(); }

private:
  bool saved_ = false;
};

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
  /**
   * G021: a scanned or pasted invite link (`pp://join/…`) goes to the chats' join prompt, which returns true when the
   * text is one; anything else stays a contact locator's business here.
   */
  void BindGroupJoinOffer(std::function<bool(const std::string& text)> offer) { offer_group_join_ = std::move(offer); }
  void BindAccountPorts(ContactsAccountPorts ports);
  /** Calls: `start_call` and `peer_calls_ready` (the chat header's own call-button condition). */
  void BindCallActions(CallActionsPorts ports);
  /** Block someone who is not a contact (saves them as a blocked contact; their id back): the chat's stranger block. */
  void BindStrangerBlocker(std::function<Roe<std::string>(const std::string& account_id)> block) {
    block_stranger_ = std::move(block);
  }
  /** Add someone who is not a contact from the chat's local directory copy; the new contact's id (or why not). */
  void BindStrangerAdder(std::function<Roe<std::string>(const std::string& account_id)> add) {
    add_stranger_ = std::move(add);
  }
  /** Where the stars ("常用联系人") are kept: the profile's preferences (device-only). */
  void BindSessionStore(SessionStore* store);

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
    /** Starred as a favourite: the phone Contacts list shows a small star after the name. */
    bool starred = false;
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

  /** One directory hit in the Add contact sheet: `index` is its place in the hits kept for the pick. */
  struct AddHitRow {
    ui::String index;
    ui::String title;
    ui::String short_id;
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
    bool has_icon = false;
    ui::String icon_src;
    ui::String avatar_letter = "?";
    int avatar_tone = 0;
    /** A group member who is not a contact (opened from a group chat): only name, photo, Account ID and Add. */
    bool member_preview = false;
    /** That member's account id (what Add contact looks up). */
    ui::String member_account;
    /**
     * The page was opened from this person's own 1:1 chat (not from a group): the chat's Block, copy ID and Add show,
     * the sheet says "Contact info", and they can be messaged (the chat is open).
     */
    bool member_from_dm = false;
    /** Starred as a favourite (the phone page's star button reads "Favorite" and is filled). */
    bool starred = false;
    /** Its Refresh runs (the button reads "Refreshing…" and a press does nothing). */
    bool refreshing = false;
    /** "Share with others" (a friend's page): the id the account text names (relay, else account), the locator the
     *  QR code and the text carry (empty without an Account ID), and which of the two choices exist. The friend's own
     *  nickname goes into both, never the remark. */
    std::string share_account_id;
    std::string share_locator;
    bool can_share_account = false;
    bool can_share_qr = false;
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
  /**
   * The contact page's / a list row's voice or video call: opens (or finds) the chat with `contact_id` and calls from
   * it, as the chat's call button does. Never reads the selected contact: the caller says whose it is. Says why (a
   * pill) when the person is blocked, cannot be reached yet, or this device is not connected yet.
   */
  void OnCallContact(const std::string& contact_id, bool video);
  /** A person who is not a contact, on the member page: asks, saves them as a blocked contact, shows their page. */
  void OnBlockMember();
  /** The page that is showing (tests read it to see that a swiped row never replaces it). */
  const ContactDetail& Selected() const { return selected_; }
  /** Add on a member page opened from a 1:1 chat: the chat menu's own add (local directory copy, the chat is adopted). */
  void OnAddMemberFromChat();
  /**
   * A button of a swiped contact row on the phone New chat page (`action`: info, voice, video, more): that contact's page,
   * a voice or video call, or the More sheet (block / unblock, remove). Every one works on `contact_id`, not on the
   * selected contact.
   */
  void OnNewChatSwipeAction(const std::string& contact_id, const std::string& action);
  /**
   * The More sheet of a swiped New chat row: Block (red) or, for a blocked contact, Unblock; and Remove contact (red).
   * Each runs the usual confirm and result pill on `contact_id`, then the New chat list is read again. Empty when there
   * is no such contact.
   */
  std::vector<ContextMenuAction> NewChatMoreActions(const std::string& contact_id, std::string* title_out = nullptr);
  /** The contact ids the New chat list shows now, in order (what its rows are built from). */
  std::vector<std::string> NewChatRowIds() const;
  void OnSelectContact(const std::string& contact_id);
  /** A group member who is not a contact: their page in its member state, pushed like a contact's page. */
  void OnShowMember(const std::string& account_id, const std::string& name, const std::string& icon_src,
                    bool from_dm = false);
  /**
   * Phones, from a group (a sender in the chat, the group info sheet, the member list): the contact's page as a 4/5
   * page sheet over where the user is (no tab or chat changes); swiped down, the user is back there. Elsewhere (the
   * desktop) OnSelectContact.
   */
  void OnSelectContactSheet(const std::string& contact_id);
  /** The same for a group member who is not a contact (its member state); elsewhere OnShowMember. */
  void OnShowMemberSheet(const std::string& account_id, const std::string& name, const std::string& icon_src,
                         bool from_dm = false);
  /** A peer's photo arrived: an open member state shows the new one (`icon_for(account id)` is its photo path). */
  void RefreshMemberIcon(const std::function<std::string(const std::string&)>& icon_for);
  /** The contact page is open as a page sheet. */
  bool DetailSheetOpen() const { return detail_sheet_layer_id_ >= 0; }
  /**
   * The group owner's "Group admin" card at the bottom of the person's page just opened from that group (phones);
   * opening any page clears it. Remove closes the sheet once the person is out.
   */
  void SetGroupAdminCard(GroupAdminCard card);
  /** The admin card is shown (tests). */
  bool GroupAdminShown() const { return admin_shown_; }
  /** The "In this group" card is shown (every group member's page; tests). */
  bool GroupRoleShown() const { return group_role_shown_; }
  /** The sheet the "Manage…" row opens, in order, the red one last (tests). */
  std::vector<ContextMenuAction> GroupAdminActions();
  void OnGroupAdminMenu();
  /** The card's Remove row, as its tap does (tests, and the view). */
  void OnGroupAdminRemove();
  void OnGroupAdminSetAdmin();
  void OnGroupAdminReinvite();
  void OnDetailDismissed();
  /** The page's star button (the view's toggle_star, and tests): stars or unstars the contact shown. */
  void OnToggleStar();
  /** The stored star order (ProfilePreferences::starred_contacts); empty without a store. */
  std::vector<std::string> Starred() const;
  /** The contact shown is starred (tests). */
  bool SelectedStarred() const { return selected_.starred; }

  /** Phone Chats "plus": pushes the full-screen New chat page (new group / add contact / contacts by letter). */
  void OpenNewChatPage();
  /** Opens (or starts) the chat with a contact, as a pick on the New chat page does (Home's Favorites). */
  void MessageContact(const std::string& contact_id);
  /**
   * Phone Contacts "+": the Add contact sheet. A search field (the directory, as you type), my account card with
   * copy and share, and rows for scan, my QR code, inviting friends and a new group.
   */
  void OpenAddContactPage();
  /** Phones (Settings > Security): the people this device blocked, each with Unblock; a tap opens their page. */
  void OpenBlockedPeople();
  /** The shell popped the blocked-people page. */
  void OnBlockedPeopleDismissed();
  /** How many saved contacts are blocked (the Settings row's count). */
  int BlockedCount() const;
  /** The New chat page left the screen (back, swipe, or a row was chosen). */
  void OnNewChatDismissed();
  /** The Add contact page was closed (the shell popped its transient, or the sheet layer went). */
  void OnAddContactDismissed();
  /** "Add contact" (New chat's row, the desktop "+" menu): the Add contact page on a phone, the paste-an-ID prompt on
   *  the desktop. Public for tests. */
  void OnAddContact();

  /** Contacts → find people; also the first-run wizard's "Find friends". */
  /**
   * An invite link the OS opened the app with (a scanned QR code, a tapped link): once unlocked, asks
   * whether to add the person it names, then adds them as a pasted invite would. Anything else is ignored.
   */
  void OfferInvite(const std::string& link);
  void OnFindSomeone();
  /** Contacts → Scan QR code. Successful friend scans still ask before saving. */
  void OnScanQr();
  /** My QR code in its own sheet (the phone scanner's button); a toast instead while there is no account. */
  void OpenMyQr();
  /** Whose code the QR page shows. Set on every open, never kept from the one before. */
  enum class QrOwner { Mine, Friend };
  /** Shows the QR page for `owner`: sets what it shows now (also when it is already open), pushes it when it is not. */
  void OpenQrPage(QrOwner owner, const std::string& name, const std::string& locator, const std::string& hint);
  QrOwner QrPageOwner() const { return qr_owner_; }
  /** What the page's code encodes, and the name under it. */
  const std::string& QrPageLocator() const { return qr_locator_; }
  std::string QrPageName() const { return qr_name_.c_str(); }
  /** Which buttons the page shows (tests): the share button, and the save button's words (empty: hidden). */
  bool QrPageCanShare() const { return qr_can_share_; }
  std::string QrPageSaveLabel() const { return qr_save_label_.c_str(); }
  /** The selected friend's QR code in the same page (his own nickname under it); a toast when he has no account. */
  void OpenSelectedFriendQr();
  /** The friend page's "Share with others" sheet: account text, card into a chat, QR code (each only when it can be). */
  std::vector<ContextMenuAction> ShareFriendActions(ui::Vector2i position);
  /** The text "Share account" hands over for the selected friend (his nickname and Account ID, never the remark). */
  std::string FriendShareText() const;
  /** Share it through the system sheet; copied with a pill where there is none. */
  void OnShareFriendAccount();
  /** The QR page's buttons: the picture through the system sheet, or saved (photos / a file). */
  void OnShareQrImage(ShareAnchor anchor);
  void OnSaveQrImage();
  /** What a finished save says (UI thread): a pill for saved / not allowed / failed, nothing for a cancel. */
  void OnQrImageSaved(ImageSaveResult result);
  /**
   * Share a contact into a chat: `share` sends the card. When it fails and `refresh_card` is set, the contact's
   * card is asked for once (its devices, then the directory — the path opening the contact uses) and, if one
   * arrives, `share` runs once more. `done(shared)` exactly once. Exposed for tests.
   */
  static void ShareContactCard(const std::function<Roe<void>()>& share,
                               const std::function<void(std::function<void(bool kept)>)>& refresh_card,
                               std::function<void(bool shared)> done);
  /** The desktop "+" menu: add contact, then find someone (the phone opens the Add contact sheet). Exposed for tests. */
  static std::vector<ContextMenuAction> AddContactMenuActions();
  /** "Add contact" (New chat's card, the Contacts "+") opens the Add contact page on a phone; the old paste-an-ID prompt
   *  is the desktop's. */
  static constexpr bool AddContactUsesPage(const bool compact) { return compact; }
  /** The Contacts tab's rows in index order (A-Z by name, Chinese by pinyin initial, then #), one section per letter. */
  static std::vector<NewChatSectionRow> ContactSections(std::vector<ContactListRow> rows);
  /** The index strip: every letter A-Z then #, marked present where `sections` has it. */
  static std::vector<IndexLetterRow> ContactIndexLetters(const std::vector<NewChatSectionRow>& sections);
  /** The section an index letter jumps to: that letter's, else the next one after it, else the last; -1 if none. */
  static int ContactSectionForLetter(const std::vector<NewChatSectionRow>& sections, char letter);
  /**
   * Scrolls the list an index strip belongs to so the section for `letter` sits at its top. `strip` is the strip
   * element; its parent holds the list: `.contacts-list-view` with `.contacts-section-header`s (`target` "contacts"), or
   * `.nc-scroll` with `.nc-section-header`s (`target` "new_chat"). The other list is not touched. Exposed for tests.
   */
  static void ScrollIndexListTo(ui::Element* strip, const std::string& target,
                                const std::vector<NewChatSectionRow>& sections, char letter);

private:
  void OnNewGroup();
  // The phone Add contact sheet.
  static void AddSearchChangedCallback(ui::DataModelHandle model, ui::Event& ev, const ui::VariantList& args);
  void OnAddSearchChanged();
  void RunAddSearch();
  void OnAddSearched(const std::string& query, uint64_t generation, Roe<std::vector<mesh::DirectoryHit>> hits);
  void OnAddPick(const std::string& index);
  void OnAddAction(const std::string& action);
  void OnAddCopyId();
  void OnAddShare(ShareAnchor anchor);
  void LoadMyAccount();
  void DirtyAddContact();
  void CloseAddContactPage();
  void OpenMyQrPage();
  void OnShareFriend(ui::Vector2i position);
  void OnMyQrDismissed();
  /** A directory hit chosen (or the only hit of a typed query): already a contact, ask first, or add. */
  void OfferHit(const mesh::DirectoryHit& hit, const std::string& query);
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
  static void RefreshContactCallback(ui::DataModelHandle model, ui::Event& ev, const ui::VariantList& args);

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
  /** Phones with layers: the detail can open as a page sheet. */
  bool DetailSheetAvailable() const;
  /** Pushes the contact page as a page sheet (once; a second person replaces the first in it). */
  void OpenContactDetailSheet();
  /** Closes every page sheet (this one and the ones under it) before a button leaves for another page. */
  void CloseSheetsBeforeLeaving();
  /** The sheet was closed (X, scrim, swipe, or code): its edits are saved, the tab's own page gets its contact back. */
  void OnDetailSheetClosed();
  /** OnDetailSheetClosed once the shell no longer has the sheet's layer. */
  void NoticeDetailSheetClosed();
  /** The selected contact cannot be messaged yet: ask for its account's card again (#409). */
  void MaybeRefreshContactCard(const std::string& contact_id);
  bool CloseContactDetailPane();
  void OnBackToList();
  void OnStartChat();
  void OnSecureMessage();
  void OnAddContactMenu(ui::Event& ev);
  /** A press or drag on a phone index strip (`target`: "contacts" or "new_chat"): jump that list to the letter under
   *  the finger, show it in the bubble. */
  void OnContactsIndexAt(ui::Event& ev, const std::string& target);
  /** The finger left the index strip: hide the bubble. */
  void OnContactsIndexEnd();
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
  /** Block the selected contact, after the usual confirm. */
  void OnBlockSelected();
  /** The same confirm for `contact_id` (a list row), which need not be the selected contact. */
  void OnBlockContactById(const std::string& contact_id);
  /**
   * Runs `run` with `contact_id` as the selected contact (the existing flows read it) and then puts the previously
   * selected page back, so a swiped row never leaks into an open contact page. Nothing runs when the contact is gone.
   */
  void WithContactSelected(const std::string& contact_id, const std::function<void()>& run);
  /** The new contact's own page replaces the member page (after Add or Block from it). */
  void ShowPersonPage(const std::string& contact_id);
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
  /** The remark field lost focus: save now and say so once (RemarkSaveNotice). */
  void OnContactFieldLeft();
  /** The edit is over (field left, page gone, typing paused): "Note saved" once if a save changed the remark. */
  void AnnounceRemarkSaved();
  /** The page's Refresh: starts the contact's refresh off the UI thread; OnContactRefreshed shows what it came to. */
  void OnRefreshContact();
  void OnContactRefreshed(const std::string& contact_id, const ContactRefreshResult& result);
  bool FlushSelectedContact();
  void UpdateMessagingEligibility(const Contact& contact);
  void DirtyAll();

  ShellChromeSnapshot ChromeSnapshot() const;
  ContactsSurfaceSnapshot BuildSurfaceSnapshot() const;
  /** Push surface snapshot to app bridge (no shell chrome knowledge). */
  void NotifySurfaceChanged();
  void ShowToast(const std::string& message, ToastDuration duration = ToastDuration::Short);
  void ShowConfirm(const std::string& title, const std::string& message, std::function<void(bool)> on_result,
                   bool destructive = false);
  void NavigateToChatSession();

  std::vector<ContactListRow> contacts_;
  std::vector<NewChatSectionRow> contact_sections_;
  std::vector<IndexLetterRow> contact_index_;
  /** The letter in the centred bubble while a finger is on the index strip; empty otherwise. */
  ui::String contact_index_bubble_;
  std::vector<NewChatSectionRow> new_chat_sections_;
  /** The New chat page's own A-Z strip (from new_chat_sections_) and whether there are any contacts at all. */
  std::vector<IndexLetterRow> new_chat_index_;
  bool new_chat_has_contacts_ = false;
  ui::String new_chat_query_;
  bool new_chat_open_ = false;
  /** The Add contact page is a full-screen transient (phones), not a page sheet layer. */
  bool add_is_transient_ = false;
  /** Blocked people, by name (OpenBlockedPeople). */
  std::vector<ContactListRow> blocked_rows_;
  bool blocked_open_ = false;
  /** The contact page shown as a page sheet (OnSelectContactSheet); -1: none. */
  int detail_sheet_layer_id_ = -1;
  RemarkSaveNotice remark_notice_;
  /** When a pause in the typing ends the remark edit (SDL ticks; 0: none pending). */
  uint64_t remark_notice_deadline_ms_ = 0;
  bool remark_wake_pending_ = false;
  void WakeForRemarkNotice();
  GroupAdminCard group_admin_;
  bool admin_shown_ = false;
  bool group_role_shown_ = false;
  ui::String group_role_text_;
  bool admin_can_reinvite_ = false;
  bool admin_can_remove_ = false;
  bool admin_can_set_admin_ = false;
  bool admin_is_admin_ = false;
  ui::String admin_reinvite_note_;
  bool admin_can_remove_added_ = false;
  ui::String admin_remove_added_label_;
  bool admin_can_reinvite_removed_ = false;
  ui::String admin_reinvite_removed_label_;
  void ClearGroupAdminCard();
  /** The contact the Contacts tab's own page showed when the sheet took the shared detail over (it comes back). */
  std::string detail_under_sheet_;
  ui::String search_query_;
  // The Add contact sheet.
  ui::String add_query_;
  std::vector<AddHitRow> add_rows_;
  std::vector<mesh::DirectoryHit> add_hits_;
  bool add_searching_ = false;
  /** The last finished search found no one. */
  bool add_no_results_ = false;
  bool add_open_ = false;
  int add_layer_id_ = -1;
  /** When the typed query is searched (SDL ticks; 0: nothing pending). */
  uint64_t add_search_deadline_ms_ = 0;
  uint64_t add_search_generation_ = 0;
  bool my_has_account_ = false;
  ui::String my_name_;
  ui::String my_account_id_;
  ui::String my_account_id_shown_;
  bool my_has_icon_ = false;
  ui::String my_icon_src_;
  ui::String my_avatar_letter_ = "?";
  int my_avatar_tone_ = 0;
  std::string my_locator_;
  std::string my_nickname_;
  ui::String my_qr_svg_rml_;
  bool can_scan_ = false;
  bool can_new_group_ = false;
  bool my_qr_open_ = false;
  QrOwner qr_owner_ = QrOwner::Mine;
  std::string qr_locator_;
  ui::String qr_name_;
  ui::String qr_hint_;
  bool qr_can_share_ = false;
  /** The second button's words ("Save to Photos", "Save image…"); empty: not shown. */
  ui::String qr_save_label_;
  int my_qr_layer_id_ = -1;
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
  std::function<bool(const std::string& text)> offer_group_join_;
  ContactsAccountPorts account_ports_;
  CallActionsPorts call_actions_;
  std::function<Roe<std::string>(const std::string& account_id)> block_stranger_;
  std::function<Roe<std::string>(const std::string& account_id)> add_stranger_;
  SessionStore* session_store_ = nullptr;
  /** Contacts whose Refresh runs. */
  std::set<std::string> refreshing_contacts_;
  /** Last: what was bound to this controller stops first. */
  pp::Lifetime lifetime_;

  static ContactsController* installed_instance_;
};

/** Whether a contact shows for a Contacts search: name, remark, nickname (case-blind or by pinyin initials) or part of an ID. */
bool ContactMatchesQuery(const Contact& contact, const std::string& query);

/** Detail form for a contact (title, remark, their nickname, ...). Exposed for tests. */
ContactsController::ContactDetail ToContactDetail(const Contact& contact, const MessagingContactsPorts& ports);
/** The page of a group member who is not a contact: name (else "Unnamed"), photo, last six of the account id. */
ContactsController::ContactDetail MemberPreviewDetail(const std::string& account_id, const std::string& name,
                                                      const std::string& icon_src, bool from_dm = false);
/** Apply the editable detail fields (the remark, capped at kMaxNicknameChars) to a copy of the contact. */
Contact BuildContactFromDetail(const Contact& existing, const ContactsController::ContactDetail& detail);

/** What a contact's Refresh says: a success (Ok) or a failure (Fail), and its words. */
struct ContactRefreshNotice {
  bool ok = false;
  std::string text;
};
/** Words for a refresh's outcome: what changed, else up to date, else which source could not be reached. */
ContactRefreshNotice DescribeContactRefresh(const ContactRefreshResult& result);

/** The blocked ones of `contacts`, sorted by name as the Contacts list sorts them. */
std::vector<Contact> BlockedContactsByName(const std::vector<Contact>& contacts);

} // namespace pbr
