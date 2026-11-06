#pragma once

#include "lib/messaging/GroupChange.h"
#include "lib/messaging/GroupE2ePayloadCodec.h"
#include "lib/messaging/GroupMembershipApply.h"
#include "lib/messaging/GroupRosterRules.h"
#include "lib/messaging/GroupRosterStatement.h"
#include "lib/messaging/IGroupRosterStore.h"
#include "lib/messaging/GroupRosterSyncCodec.h"
#include "common/Error.h"

#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * The rules of the owner-signed roster (G012), the changes members and admins sign on top of it (G018), and a
 * group's member check (G013), over the roster store. The workflow sends; these decide.
 */
namespace group_roster_sync {

/** The statement a group holds, if any. */
std::optional<GroupRosterStatement> Held(const GroupMetadata& metadata);

// ---- G018: the changes on top of the statement ----

/** At most this many unfolded changes per group: past it a new one is refused until the owner folds. */
inline constexpr size_t kMaxHeldChanges = 512;

/** The changes a group holds, parsed (one that no longer parses is skipped; ApplyView drops it). */
std::vector<GroupChange> HeldChanges(IGroupRosterStore& roster, const std::string& group_id);

/** What ApplyView did. */
struct GroupViewApplied {
  /** The roster as it stands now; none when it was left alone (no statement held, or the group ended). */
  std::optional<GroupRosterView> view;
  /** Changes that count now and had no line in the group's chat yet (now marked as having one): write theirs. */
  std::vector<GroupChange> newly_applied;
  /** Members the roster lost and gained. */
  std::vector<std::string> gone;
  std::vector<std::string> joined;
  /** The group's shared title changed (a rename change, or a statement naming a newer title): the chat's follows. */
  std::optional<std::string> new_title;
};

/**
 * G018, the one apply path: whenever the statement or the held changes change, the roster becomes the latest
 * statement plus the changes (CheckChanges, ComputeRosterChecked) — members with their roles, added_by and
 * joined_at (the contact ids we had kept), the owner, the epoch, the preset, a newer title. The changes the view
 * drops (folded, too old, another group's, forged) are dropped; a dissolved group (G016) keeps none. With no
 * statement held, or one older than the roster's epoch (a roster event applied whose statement was not kept), the
 * roster is left as it is.
 */
Roe<GroupViewApplied> ApplyView(IGroupRosterStore& roster, const std::string& group_id, const GroupCardLookup& cards,
                                int64_t now_ms);

/** How a change was taken (HoldChange). */
enum class ChangeHeld {
  /** It proves itself (against a card we hold or the one it carries): kept. */
  Proved,
  /** No card we have lists its device: kept, to verify once the card arrives (ask for it). */
  NoCard,
  /** Forged, damaged, for a group we do not hold (or one that ended), or the group holds too many: not kept. */
  Refused,
};

/**
 * Keep `change` for its group as HoldChange decides, `line_shown` when its line is already written (our own). A copy
 * already held under its id is replaced only by one that carries the signer's card (it proves more).
 */
Roe<ChangeHeld> HoldChange(IGroupRosterStore& roster, const GroupChange& change, const GroupCardLookup& cards,
                           int64_t now_ms, bool line_shown = false);

/**
 * Changes that ride one message (a member check reply, a join bundle), each signer's card at most once: every change
 * of a signer is held with the card one of them carried, so it still proves itself once that one is folded and
 * dropped. Each is held as HoldChange decides (`line_shown` for all of them).
 */
Roe<void> HoldCarried(IGroupRosterStore& roster, const std::vector<GroupChange>& changes, const GroupCardLookup& cards,
                      int64_t now_ms, bool line_shown = false);

/**
 * G019, the newcomer's check before it keeps anything of a join bundle: `statement` verifies for the owner this device
 * knows for the group, and the roster it gives with the changes carried (and any held) lists `identity`. Nothing is
 * written, so a bundle that does not prove the join leaves the group as accepting left it.
 */
Roe<void> CheckJoin(IGroupRosterStore& roster, const GroupRosterStatement& statement,
                    const std::vector<GroupChange>& changes, const GroupCardLookup& cards, int64_t now_ms,
                    const std::string& identity);

/** A change received: HoldChange, then ApplyView when it was kept. */
struct ChangeReceived {
  ChangeHeld held = ChangeHeld::Refused;
  GroupViewApplied applied;
};
Roe<ChangeReceived> ReceiveChange(IGroupRosterStore& roster, const GroupChange& change, const GroupCardLookup& cards,
                                  int64_t now_ms);

/**
 * Our `leave` (G018 item 2), unsigned: `adds` the held adds that brought us in, `keeps` our own held changes; its base
 * epoch the statement we hold. The owner does not leave (G009).
 */
Roe<GroupChange> DraftLeave(IGroupRosterStore& roster, const std::string& group_id, const std::string& local_identity,
                            const std::string& local_device, int64_t now_ms);
/**
 * Our `remove` of `member` (G018, G020), unsigned: `adds` the held adds of the member we saw, `keeps` the member's
 * held changes. Errors when the roster we hold does not let us (GroupAllows: the owner anyone, an admin members).
 */
Roe<GroupChange> DraftRemove(IGroupRosterStore& roster, const std::string& group_id, const std::string& local_identity,
                             const std::string& local_device, const std::string& member, int64_t now_ms);

/**
 * G019: our `add` of `member` for the invite `invite_nonce`, unsigned. Errors unless the roster we hold lets us
 * (GroupAllows Invite under the preset: `kGroupInviteNotAllowedError`), they are not in already
 * (`kGroupAlreadyMemberError`) and, for someone removed (G020 5, R2), we may re-admit them — the add then names the
 * removal in `readmits`; a member may not (`kGroupInviteRemovedError`).
 */
Roe<GroupChange> DraftAdd(IGroupRosterStore& roster, const std::string& group_id, const std::string& local_identity,
                          const std::string& local_device, const std::string& member, const std::string& invite_nonce,
                          const GroupCardLookup& cards, int64_t now_ms);
/**
 * G017: our `rename` to `title` (1–256 bytes), unsigned, one past the title version the roster holds. Errors unless the
 * preset lets our role rename (`kGroupRenameNotAllowedError`).
 */
Roe<GroupChange> DraftRename(IGroupRosterStore& roster, const std::string& group_id, const std::string& local_identity,
                             const std::string& local_device, const std::string& title, const GroupCardLookup& cards,
                             int64_t now_ms);

/**
 * G020 6, "Remove everyone {name} added": the members `account` brought in (their added_by) who are still in and whom
 * `local_identity` may remove (GroupAllows: the owner anyone but itself, an admin members), sorted. None for a role
 * that removes no one.
 */
std::vector<std::string> RemovableAddedBy(IGroupRosterStore& roster, const std::string& group_id,
                                          const std::string& local_identity, const std::string& account);
/** G020 6, "Re-invite everyone {name} removed": the accounts `account` removed who are still out, sorted. */
std::vector<std::string> RemovedBy(IGroupRosterStore& roster, const std::string& group_id, const std::string& account,
                                   const GroupCardLookup& cards, int64_t now_ms);

/** The roster as this device holds it now: the held statement plus the held changes; none without a statement. */
std::optional<GroupRosterView> CurrentView(IGroupRosterStore& roster, const std::string& group_id,
                                           const GroupCardLookup& cards, int64_t now_ms);
/**
 * G020 5: `member`'s entry on the group's removed list, as the roster holds it now (the statement's, else a removal
 * among the held changes that counts) — what a re-admit names. None for someone not removed.
 */
std::optional<GroupRemovedMember> RemovedEntry(IGroupRosterStore& roster, const std::string& group_id,
                                               const std::string& member, const GroupCardLookup& cards, int64_t now_ms);

/**
 * G018 item 4, the owner's side: the view a fold starts from — the held statement (or, before the first one, the
 * roster as `rows` and `metadata` state it) plus the held changes.
 */
GroupRosterView FoldBase(IGroupRosterStore& roster, const GroupMetadata& metadata,
                         const std::vector<GroupRosterStatementMember>& rows, const GroupCardLookup& cards,
                         int64_t now_ms, GroupRosterStatement& base);

/**
 * The owner's next statement over `rows` (the roster as the owner's device holds it, its own edits in), unsigned:
 * the epoch at least `metadata`'s and the held statement's (one past it when what it says changed), `metadata`'s
 * preset and title, the owner chain as given, and the fold (FoldInto): the removed the changes leave (not
 * re-added), the live links of makers still in, the changes folded, the change floor raised.
 */
GroupRosterStatement OwnerDraft(IGroupRosterStore& roster, const GroupMetadata& metadata,
                                std::vector<GroupRosterStatementMember> rows, std::vector<GroupOwnerTransfer> chain,
                                const std::string& owner_identity, const std::string& owner_key_b64,
                                const GroupCardLookup& cards, int64_t now_ms);

/** The group holds a change its owner would fold now (FoldsIn). */
bool HoldsFoldable(IGroupRosterStore& roster, const std::string& group_id, const GroupCardLookup& cards,
                   int64_t now_ms);

/** The groups holding a change signed by `signer` (its card just arrived: they are worth a recompute). */
std::vector<std::string> GroupsWithChangesBy(IGroupRosterStore& roster, const std::string& signer);

// ---- G018 item 5: keeping members in step ----

/** What our group copies say of our roster: the statement's epoch, the hash of the changes we hold. */
GroupCopyState CopyState(IGroupRosterStore& roster, const std::string& group_id);

/**
 * A copy's sender holds what we lack: a newer statement, or the same one with other changes. A sender behind us is
 * not a reason to ask (it asks when our copy reaches it).
 */
bool NeedsCatchUp(const GroupCopyState& ours, const GroupCopyState& theirs);

/** Ask at most once per group per kCatchUpGapMs, however many copies a burst brings. Thread-safe. */
class CatchUpLimiter {
public:
  static constexpr int64_t kCatchUpGapMs = 30'000;
  bool Allow(const std::string& group_id, int64_t now_ms);

private:
  std::mutex mutex_;
  std::map<std::string, int64_t> last_;
};

/**
 * A catch-up check (G018 item 5): a request carrying our copy state, its id the group's open one, while the group
 * stays active (sends go on). None for a group that is not active (a check of its own runs, or it is over).
 */
Roe<std::optional<group_roster_sync_codec::RosterRequest>> BeginCatchUp(IGroupRosterStore& roster,
                                                                        const std::string& group_id, int64_t now_ms,
                                                                        const std::string& after = {});

/**
 * The changes `request`'s asker lacks, into `reply`: when its changes hash is not ours, the ones we hold after its
 * `after`, in id order, bounded (kMaxReplyChanges, kReplyChangesBudgetBytes; `more` when cut), each signer's card
 * carried once.
 */
void AddChangesTo(IGroupRosterStore& roster, const group_roster_sync_codec::RosterRequest& request,
                  group_roster_sync_codec::RosterReply& reply);

/**
 * G019: the changes a newcomer's join bundle carries — every change the group holds, `first` (the add that brings
 * them in) leading and the rest in id order, bounded as a member check reply is (kMaxReplyChanges,
 * kReplyChangesBudgetBytes; `more` when cut: the newcomer asks the inviter for the rest), each signer's card once.
 */
group_roster_sync_codec::CarriedChanges ChangesForNewcomer(IGroupRosterStore& roster, const std::string& group_id,
                                                           const std::string& first);

/**
 * Keep `statement` as the group's latest when it is valid for the owner we know and wins over the one
 * held (Pick: a former owner cannot outbid its successor, then the highest epoch). True when kept.
 */
Roe<bool> KeepStatement(IGroupRosterStore& roster, const GroupRosterStatement& statement);

/**
 * A statement from the owner (`group_roster_statement`, or one a roster event carries): kept when it wins, then the
 * roster recomputed over it (ApplyView). True when kept; `applied` gets what the recompute did.
 */
Roe<bool> KeepAndCatchUp(IGroupRosterStore& roster, const GroupRosterStatement& statement, const GroupCardLookup& cards,
                         int64_t now_ms, GroupViewApplied* applied = nullptr);

/** A member check to send: the request, and who to ask (the owner and the members we know, not us). */
struct Check {
  group_roster_sync_codec::RosterRequest request;
  std::vector<std::string> ask;
};

/** Start (or restart) the group's member check: state checking, a new request id, our copy state in the request. */
Roe<Check> BeginCheck(IGroupRosterStore& roster, const std::string& group_id, const std::string& local_identity,
                      int64_t now_ms);

/**
 * A member's answer to `asker`'s check: the statement it holds, the changes the asker lacks (AddChangesTo), and
 * removed when its roster no longer has the asker. Nullopt when we hold nothing to say (not a group we know). A
 * member still checking itself answers with its statement only (its roster may be stale).
 */
Roe<std::optional<group_roster_sync_codec::RosterReply>> Answer(IGroupRosterStore& roster,
                                                                const group_roster_sync_codec::RosterRequest& request,
                                                                const std::string& asker);

enum class ReplyOutcome {
  /** Not for our open check, unverifiable, or it tells us nothing yet. */
  Ignored,
  Active,
  Removed,
  /** The answer was the owner's dissolve (G016): the group is over, its chat stays. */
  Dissolved,
};

/**
 * Apply `replier`'s reply to our check (G013) or catch-up (G018 item 5): only one for our latest request; a
 * statement must verify for the owner we know. The winning statement is kept and the changes the reply carries are
 * held (each proving itself), then the roster recomputed (ApplyView); the check ends active when the roster lists
 * us, removed when not. With no statement, only the owner's `removed` counts. Later replies to the same request
 * still apply (the owner's own reply wins when it arrives). A statement that is the owner's dissolve (G016) ends the
 * group (`Dissolved`; what it voided in `dissolved`). A reply with `more` is the caller's to ask again.
 */
Roe<ReplyOutcome> ApplyReply(IGroupRosterStore& roster, const group_roster_sync_codec::RosterReply& reply,
                             const std::string& local_identity, const std::string& replier,
                             const GroupCardLookup& cards, int64_t now_ms, GroupDissolvedApplied* dissolved = nullptr,
                             GroupViewApplied* applied = nullptr);

/** An unanswered check is asked again when a group message shows a member is online (G013 amendment). */
inline constexpr int64_t kCheckRetryMs = 15000;

/** Why a group send was refused (i18n keys the GUI words). */
inline constexpr const char kGroupCheckingError[] = "chat.group.checking_members";
inline constexpr const char kGroupRemovedError[] = "chat.group.removed";
/** A group the owner ended (G016) refuses sends. */
inline constexpr const char kGroupDissolvedError[] = "chat.group.dissolved";

/** A group may send only while active (not checking its members, not removed, not dissolved). */
bool MaySend(const GroupMetadata& metadata);

/**
 * Check `request_id` is still the one waiting for an answer: the group is checking and no newer check began (that one
 * has its own re-asks). What a timed re-ask asks before it asks again.
 */
bool StillAwaiting(const GroupMetadata& metadata, const std::string& request_id);

} // namespace group_roster_sync

} // namespace pbr
