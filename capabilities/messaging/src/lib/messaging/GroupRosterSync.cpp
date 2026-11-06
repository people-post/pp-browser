#include "lib/messaging/GroupRosterSync.h"

#include "lib/messaging/GroupMembershipApply.h"
#include "lib/messaging/GroupPermissions.h"

#include "common/Utilities.h"
#include "common/ValueJson.h"

#include <algorithm>
#include <set>
#include "common/PbrCompat.h"

namespace pbr {
namespace group_roster_sync {
namespace {

Roe<GroupMetadata> MetadataOf(IGroupRosterStore& roster, const std::string& group_id) {
  auto metadata = roster.LoadMetadata(group_id);
  if (!metadata) {
    return metadata.error();
  }
  if (!metadata->has_value()) {
    return Error("Group metadata not found: " + group_id);
  }
  return **metadata;
}

std::string ToJsonText(const GroupRosterStatement& statement) {
  return DumpJson(group_roster_statement::ToJson(statement));
}

/** The statement among held and `candidate` that wins (candidate is valid; held was when kept). */
bool CandidateWins(const std::optional<GroupRosterStatement>& held, const GroupRosterStatement& candidate) {
  if (!held) {
    return true;
  }
  auto picked = group_roster_statement::Pick({*held, candidate});
  return picked && picked->owner_signature_b64 == candidate.owner_signature_b64 &&
         picked->roster_epoch == candidate.roster_epoch;
}

std::optional<GroupChange> ParseChange(const StoredGroupChange& stored) {
  auto json = TryParseObject(stored.change_json);
  if (!json) {
    return std::nullopt;
  }
  auto change = group_change::FromJson(*json);
  if (!change || change->group_id != stored.group_id) {
    return std::nullopt;
  }
  return std::move(*change);
}

/** The roster rows become the view's members; the contact ids we had stay. */
Roe<void> WriteRows(IGroupRosterStore& roster, const std::string& group_id, const GroupRosterView& view,
                    GroupViewApplied& applied) {
  auto current = roster.ListMembers(group_id);
  if (!current) {
    return current.error();
  }
  for (const GroupRosterMember& m : *current) {
    if (!view.Lists(m.member_identity)) {
      if (auto removed = roster.RemoveMember(group_id, m.member_identity); !removed) {
        return removed.error();
      }
      applied.gone.push_back(m.member_identity);
    }
  }
  for (const GroupRosterViewMember& m : view.members) {
    const auto had = std::find_if(current->begin(), current->end(),
                                  [&](const GroupRosterMember& r) { return r.member_identity == m.identity; });
    GroupRosterMember row{.member_identity = m.identity,
                          .contact_id = had == current->end() ? std::string() : had->contact_id,
                          .role = m.role,
                          .joined_at = m.joined_at,
                          .added_by = m.added_by};
    if (had == current->end()) {
      applied.joined.push_back(m.identity);
    } else if (had->role == row.role && had->joined_at == row.joined_at && had->added_by == row.added_by) {
      continue; // unchanged
    }
    if (auto upserted = roster.UpsertMember(group_id, row); !upserted) {
      return upserted.error();
    }
  }
  return {};
}

/** Drop what the view drops; mark (and return) the changes that count now and have no line yet. */
Roe<void> SettleHeld(IGroupRosterStore& roster, const std::string& group_id, std::vector<StoredGroupChange>& stored,
                     const std::vector<std::string>& unreadable, const GroupRosterView& view,
                     GroupViewApplied& applied) {
  std::vector<std::string> drop = unreadable;
  for (const GroupChangeVerdict& v : view.verdicts) {
    if (v.status == GroupChangeStatus::Dropped) {
      drop.push_back(v.id);
    }
  }
  if (auto dropped = roster.DeleteGroupChanges(group_id, drop); !dropped) {
    return dropped.error();
  }
  for (StoredGroupChange& row : stored) {
    const GroupChangeVerdict* verdict = view.Verdict(row.change_id);
    if (row.line_shown || !verdict || verdict->status != GroupChangeStatus::Applied) {
      continue;
    }
    auto change = ParseChange(row);
    if (!change) {
      continue;
    }
    row.line_shown = true;
    if (auto kept = roster.UpsertGroupChange(row); !kept) {
      return kept.error();
    }
    applied.newly_applied.push_back(std::move(*change));
  }
  return {};
}

/** Ids of held adds naming `member`, and of held changes signed by `signer` (a leave or remove names them). */
std::vector<std::string> AddsOf(const std::vector<GroupChange>& held, const std::string& member) {
  std::vector<std::string> ids;
  for (const GroupChange& c : held) {
    if (c.kind == GroupChangeKind::Add && c.member == member) {
      ids.push_back(group_change::Id(c));
    }
  }
  return ids;
}

std::vector<std::string> SignedBy(const std::vector<GroupChange>& held, const std::string& signer) {
  std::vector<std::string> ids;
  for (const GroupChange& c : held) {
    if (c.signer == signer) {
      ids.push_back(group_change::Id(c));
    }
  }
  return ids;
}

/** The kept bound (kMaxNamedChanges) on a list a change names. */
void Bound(std::vector<std::string>& ids) {
  std::sort(ids.begin(), ids.end());
  ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
  if (ids.size() > group_change::kMaxNamedChanges) {
    ids.resize(group_change::kMaxNamedChanges);
  }
}

Roe<GroupChange> DraftOn(IGroupRosterStore& roster, const std::string& group_id, const std::string& local_identity,
                         const std::string& local_device, const GroupChangeKind kind, const int64_t now_ms) {
  auto metadata = MetadataOf(roster, group_id);
  if (!metadata) {
    return metadata.error();
  }
  if (metadata->sync_state != GroupSyncState::Active) {
    return Error(metadata->sync_state == GroupSyncState::Dissolved ? kGroupDissolvedError
                 : metadata->sync_state == GroupSyncState::Removed ? kGroupRemovedError
                                                                   : kGroupCheckingError);
  }
  GroupChange change;
  change.group_id = group_id;
  const std::optional<GroupRosterStatement> held = Held(*metadata);
  change.base_epoch = held ? held->roster_epoch : metadata->roster_epoch;
  change.signer = local_identity;
  change.signer_device = local_device;
  change.at = now_ms;
  change.kind = kind;
  return change;
}

} // namespace

std::optional<GroupRosterStatement> Held(const GroupMetadata& metadata) {
  if (metadata.roster_statement_json.empty()) {
    return std::nullopt;
  }
  auto json = TryParseObject(metadata.roster_statement_json);
  if (!json) {
    return std::nullopt;
  }
  auto statement = group_roster_statement::FromJson(*json);
  if (!statement) {
    return std::nullopt;
  }
  return std::move(*statement);
}

std::vector<GroupChange> HeldChanges(IGroupRosterStore& roster, const std::string& group_id) {
  std::vector<GroupChange> changes;
  auto stored = roster.ListGroupChanges(group_id);
  if (!stored) {
    return changes;
  }
  for (const StoredGroupChange& row : *stored) {
    if (auto change = ParseChange(row)) {
      changes.push_back(std::move(*change));
    }
  }
  return changes;
}

Roe<GroupViewApplied> ApplyView(IGroupRosterStore& roster, const std::string& group_id, const GroupCardLookup& cards,
                                const int64_t now_ms) {
  auto lock = IGroupRosterStore::LockForUpdate();
  GroupViewApplied applied;
  auto metadata = MetadataOf(roster, group_id);
  if (!metadata) {
    return metadata.error();
  }
  auto stored = roster.ListGroupChanges(group_id);
  if (!stored) {
    return stored.error();
  }
  if (metadata->sync_state == GroupSyncState::Dissolved) {
    std::vector<std::string> all;
    for (const StoredGroupChange& row : *stored) {
      all.push_back(row.change_id);
    }
    if (auto dropped = roster.DeleteGroupChanges(group_id, all); !dropped) {
      return dropped.error();
    }
    return applied; // G016: an ended group holds no roster and no changes
  }
  const std::optional<GroupRosterStatement> held = Held(*metadata);
  if (!held || held->roster_epoch < metadata->roster_epoch) {
    return applied;
  }
  std::vector<GroupChange> changes;
  std::vector<std::string> unreadable;
  for (const StoredGroupChange& row : *stored) {
    if (auto change = ParseChange(row)) {
      changes.push_back(std::move(*change));
    } else {
      unreadable.push_back(row.change_id);
    }
  }
  GroupRosterView view = ComputeRosterChecked(*held, CheckChanges(changes, cards, now_ms), now_ms);
  if (auto settled = SettleHeld(roster, group_id, *stored, unreadable, view, applied); !settled) {
    return settled.error();
  }
  if (auto written = WriteRows(roster, group_id, view, applied); !written) {
    return written.error();
  }
  auto now = MetadataOf(roster, group_id);
  if (!now) {
    return now.error();
  }
  GroupMetadata updated = *now;
  updated.owner_identity = held->owner_identity;
  updated.roster_epoch = held->roster_epoch;
  updated.policy.preset = held->preset;
  if (!view.title.empty() && view.title_version > updated.title_version) {
    updated.title = view.title;
    updated.title_version = view.title_version;
    if (updated.title != now->title) {
      applied.new_title = updated.title;
    }
  }
  const bool same = updated.owner_identity == now->owner_identity && updated.roster_epoch == now->roster_epoch &&
                    updated.policy.preset == now->policy.preset && updated.title == now->title &&
                    updated.title_version == now->title_version;
  if (!same) { // an unchanged roster writes nothing (each write republishes the account locker)
    if (auto saved = roster.UpsertMetadata(updated); !saved) {
      return saved.error();
    }
  }
  applied.view = std::move(view);
  return applied;
}

Roe<ChangeHeld> HoldChange(IGroupRosterStore& roster, const GroupChange& change, const GroupCardLookup& cards,
                           const int64_t now_ms, const bool line_shown) {
  auto lock = IGroupRosterStore::LockForUpdate();
  auto metadata = roster.LoadMetadata(change.group_id);
  if (!metadata) {
    return metadata.error();
  }
  if (!*metadata || (*metadata)->sync_state == GroupSyncState::Dissolved) {
    return ChangeHeld::Refused;
  }
  const std::vector<CheckedGroupChange> checked = CheckChanges({change}, cards, now_ms);
  if (checked.empty() ||
      (checked.front().proof != GroupChangeProof::Proved && checked.front().proof != GroupChangeProof::NoCard)) {
    return ChangeHeld::Refused;
  }
  const std::string id = checked.front().id;
  auto stored = roster.ListGroupChanges(change.group_id);
  if (!stored) {
    return stored.error();
  }
  const auto had =
      std::find_if(stored->begin(), stored->end(), [&](const StoredGroupChange& row) { return row.change_id == id; });
  if (had == stored->end() && stored->size() >= kMaxHeldChanges) {
    return ChangeHeld::Refused;
  }
  const ChangeHeld held = checked.front().proof == GroupChangeProof::Proved ? ChangeHeld::Proved : ChangeHeld::NoCard;
  if (had != stored->end()) {
    auto existing = ParseChange(*had);
    if (existing && existing->signer_card && !change.signer_card) {
      if (line_shown && !had->line_shown) {
        StoredGroupChange row = *had;
        row.line_shown = true;
        return roster.UpsertGroupChange(row) ? Roe<ChangeHeld>(held) : Roe<ChangeHeld>(Error("change not kept"));
      }
      return held; // the copy we hold proves at least as much
    }
  }
  StoredGroupChange row{.group_id = change.group_id,
                        .change_id = id,
                        .change_json = DumpJson(group_change::ToJson(change)),
                        .received_at = now_ms,
                        .line_shown = line_shown || (had != stored->end() && had->line_shown)};
  if (auto kept = roster.UpsertGroupChange(row); !kept) {
    return kept.error();
  }
  return held;
}

Roe<void> HoldCarried(IGroupRosterStore& roster, const std::vector<GroupChange>& changes, const GroupCardLookup& cards,
                      const int64_t now_ms, const bool line_shown) {
  auto lock = IGroupRosterStore::LockForUpdate();
  std::map<std::string, AccountCard> carried;
  for (const GroupChange& change : changes) {
    if (change.signer_card) {
      carried.emplace(change.signer, *change.signer_card);
    }
  }
  for (GroupChange change : changes) {
    if (auto card = carried.find(change.signer); !change.signer_card && card != carried.end()) {
      change.signer_card = card->second;
    }
    if (auto held = HoldChange(roster, change, cards, now_ms, line_shown); !held) {
      return held.error();
    }
  }
  return {};
}

Roe<void> CheckJoin(IGroupRosterStore& roster, const GroupRosterStatement& statement,
                    const std::vector<GroupChange>& changes, const GroupCardLookup& cards, const int64_t now_ms,
                    const std::string& identity) {
  auto metadata = MetadataOf(roster, statement.group_id);
  if (!metadata) {
    return metadata.error();
  }
  if (statement.dissolved) {
    return Error("the group was dissolved");
  }
  if (auto valid = group_roster_statement::Verify(statement, metadata->owner_identity); !valid) {
    return valid.error();
  }
  std::vector<GroupChange> all = HeldChanges(roster, statement.group_id);
  std::map<std::string, AccountCard> carried;
  for (const GroupChange& change : changes) {
    if (change.signer_card) {
      carried.emplace(change.signer, *change.signer_card);
    }
  }
  for (GroupChange change : changes) {
    if (auto card = carried.find(change.signer); !change.signer_card && card != carried.end()) {
      change.signer_card = card->second;
    }
    all.push_back(std::move(change));
  }
  const GroupRosterView view = ComputeRosterChecked(statement, CheckChanges(all, cards, now_ms), now_ms);
  if (!view.Lists(identity)) {
    return Error("the roster it proves does not have us");
  }
  return {};
}

Roe<ChangeReceived> ReceiveChange(IGroupRosterStore& roster, const GroupChange& change, const GroupCardLookup& cards,
                                  const int64_t now_ms) {
  auto lock = IGroupRosterStore::LockForUpdate();
  ChangeReceived received;
  auto held = HoldChange(roster, change, cards, now_ms);
  if (!held) {
    return held.error();
  }
  received.held = *held;
  if (*held == ChangeHeld::Refused) {
    return received;
  }
  auto applied = ApplyView(roster, change.group_id, cards, now_ms);
  if (!applied) {
    return applied.error();
  }
  received.applied = std::move(*applied);
  return received;
}

Roe<GroupChange> DraftLeave(IGroupRosterStore& roster, const std::string& group_id, const std::string& local_identity,
                            const std::string& local_device, const int64_t now_ms) {
  auto change = DraftOn(roster, group_id, local_identity, local_device, GroupChangeKind::Leave, now_ms);
  if (!change) {
    return change.error();
  }
  auto metadata = MetadataOf(roster, group_id);
  if (!metadata) {
    return metadata.error();
  }
  if (metadata->owner_identity == local_identity) {
    return Error("Owner must transfer ownership before leaving");
  }
  const std::vector<GroupChange> held = HeldChanges(roster, group_id);
  change->adds = AddsOf(held, local_identity);
  change->keeps = SignedBy(held, local_identity);
  Bound(change->adds);
  Bound(change->keeps);
  return change;
}

Roe<GroupChange> DraftRemove(IGroupRosterStore& roster, const std::string& group_id, const std::string& local_identity,
                             const std::string& local_device, const std::string& member, const int64_t now_ms) {
  auto change = DraftOn(roster, group_id, local_identity, local_device, GroupChangeKind::Remove, now_ms);
  if (!change) {
    return change.error();
  }
  if (member.empty() || member == local_identity) {
    return Error("Choose someone else to remove");
  }
  auto metadata = MetadataOf(roster, group_id);
  if (!metadata) {
    return metadata.error();
  }
  auto self = roster.FindMember(group_id, local_identity);
  auto target = roster.FindMember(group_id, member);
  if (!self || !target) {
    return !self ? self.error() : target.error();
  }
  if (!*self || !*target) {
    return Error("Not a member of this group");
  }
  if (!GroupAllows(GroupAction::Remove, (*self)->role, metadata->policy.preset, (*target)->role)) {
    return Error("Only the owner removes admins; admins remove members");
  }
  const std::vector<GroupChange> held = HeldChanges(roster, group_id);
  change->member = member;
  change->adds = AddsOf(held, member);
  change->keeps = SignedBy(held, member);
  Bound(change->adds);
  Bound(change->keeps);
  return change;
}

std::optional<GroupRosterView> CurrentView(IGroupRosterStore& roster, const std::string& group_id,
                                           const GroupCardLookup& cards, const int64_t now_ms) {
  auto metadata = roster.LoadMetadata(group_id);
  if (!metadata || !*metadata) {
    return std::nullopt;
  }
  const std::optional<GroupRosterStatement> held = Held(**metadata);
  if (!held) {
    return std::nullopt;
  }
  return ComputeRosterChecked(*held, CheckChanges(HeldChanges(roster, group_id), cards, now_ms), now_ms);
}

std::optional<GroupRemovedMember> RemovedEntry(IGroupRosterStore& roster, const std::string& group_id,
                                               const std::string& member, const GroupCardLookup& cards,
                                               const int64_t now_ms) {
  auto metadata = roster.LoadMetadata(group_id);
  if (!metadata || !*metadata) {
    return std::nullopt;
  }
  // The statement's entry first: a re-admit of someone it lists must name exactly that removal (R2).
  if (const std::optional<GroupRosterStatement> held = Held(**metadata)) {
    for (const GroupRemovedMember& r : held->removed) {
      if (r.identity == member) {
        return r;
      }
    }
  }
  const std::optional<GroupRosterView> view = CurrentView(roster, group_id, cards, now_ms);
  if (!view || view->Lists(member)) {
    return std::nullopt;
  }
  for (const GroupRemovedMember& r : view->removed) {
    if (r.identity == member) {
      return r;
    }
  }
  return std::nullopt;
}

namespace {

/** Our role in the group as the roster we hold has it (nullopt: not in it). */
std::optional<MemberRole> OwnRole(IGroupRosterStore& roster, const GroupMetadata& metadata,
                                  const std::string& local_identity) {
  if (metadata.owner_identity == local_identity) {
    return MemberRole::Owner;
  }
  auto self = roster.FindMember(metadata.group_id, local_identity);
  if (!self || !*self) {
    return std::nullopt;
  }
  return (*self)->role;
}

/** An add of `member` by us: for the invite `invite_nonce`, or for a join by the link `link_id` (G021). */
Roe<GroupChange> DraftAddOf(IGroupRosterStore& roster, const std::string& group_id, const std::string& local_identity,
                            const std::string& local_device, const std::string& member, const std::string& invite_nonce,
                            const std::string& link_id, const GroupCardLookup& cards, const int64_t now_ms) {
  auto change = DraftOn(roster, group_id, local_identity, local_device, GroupChangeKind::Add, now_ms);
  if (!change) {
    return change.error();
  }
  if (member.empty() || member == local_identity || invite_nonce.empty() == link_id.empty()) {
    return Error("An add names someone else and the invite or link it answers");
  }
  auto metadata = MetadataOf(roster, group_id);
  if (!metadata) {
    return metadata.error();
  }
  const std::optional<MemberRole> role = OwnRole(roster, *metadata, local_identity);
  if (!role || !GroupAllows(GroupAction::Invite, *role, metadata->policy.preset)) {
    return Error(kGroupInviteNotAllowedError);
  }
  if (auto in = roster.IsMember(group_id, member); in && *in) {
    return Error(kGroupAlreadyMemberError);
  }
  if (const auto removed = RemovedEntry(roster, group_id, member, cards, now_ms)) {
    // R2: only the owner or an admin brings them back, by an invite (a join by link is never a re-admit, G021).
    if (!link_id.empty() || !GroupAllows(GroupAction::Readmit, *role, metadata->policy.preset)) {
      return Error(kGroupInviteRemovedError);
    }
    change->readmits = removed->removal;
  }
  change->member = member;
  change->invite_nonce = invite_nonce;
  change->link_id = link_id;
  return change;
}

/** The link `link_id` as the roster holds it now, live or ended; none when it does not know it. */
std::optional<GroupInviteLink> KnownLink(const GroupRosterView& view, const std::string& link_id) {
  if (const GroupInviteLink* live = view.LiveLink(link_id)) {
    return *live;
  }
  if (const GroupEndedLink* ended = view.EndedLink(link_id)) {
    return ended->link;
  }
  return std::nullopt;
}

} // namespace

Roe<GroupChange> DraftAdd(IGroupRosterStore& roster, const std::string& group_id, const std::string& local_identity,
                          const std::string& local_device, const std::string& member, const std::string& invite_nonce,
                          const GroupCardLookup& cards, const int64_t now_ms) {
  if (invite_nonce.empty()) {
    return Error("An add names the invite it answers");
  }
  return DraftAddOf(roster, group_id, local_identity, local_device, member, invite_nonce, {}, cards, now_ms);
}

Roe<GroupChange> DraftLinkJoin(IGroupRosterStore& roster, const std::string& group_id,
                               const std::string& local_identity, const std::string& local_device,
                               const std::string& member, const std::string& link_id, const GroupCardLookup& cards,
                               const int64_t now_ms) {
  if (link_id.empty()) {
    return Error("A join by link names its link");
  }
  return DraftAddOf(roster, group_id, local_identity, local_device, member, {}, link_id, cards, now_ms);
}

Roe<GroupChange> DraftLinkAdd(IGroupRosterStore& roster, const std::string& group_id, const std::string& local_identity,
                              const std::string& local_device, const std::string& link_id,
                              const std::string& secret_hash, const int64_t expires_at, const uint64_t max_uses,
                              const GroupCardLookup& cards, const int64_t now_ms) {
  auto change = DraftOn(roster, group_id, local_identity, local_device, GroupChangeKind::LinkAdd, now_ms);
  if (!change) {
    return change.error();
  }
  if (link_id.empty() || secret_hash.empty() || expires_at <= now_ms) {
    return Error("A link has its id, its secret's hash and an expiry to come");
  }
  auto metadata = MetadataOf(roster, group_id);
  if (!metadata) {
    return metadata.error();
  }
  const std::optional<MemberRole> role = OwnRole(roster, *metadata, local_identity);
  if (!role || !GroupAllows(GroupAction::MakeInviteLink, *role, metadata->policy.preset)) {
    return Error(kGroupLinkNotAllowedError);
  }
  if (const std::optional<GroupRosterView> view = CurrentView(roster, group_id, cards, now_ms);
      view && KnownLink(*view, link_id)) {
    return Error("That link id is taken");
  }
  change->link_id = link_id;
  change->secret_hash = secret_hash;
  change->expires_at = expires_at;
  change->max_uses = max_uses;
  return change;
}

Roe<GroupChange> DraftLinkRevoke(IGroupRosterStore& roster, const std::string& group_id,
                                 const std::string& local_identity, const std::string& local_device,
                                 const std::string& link_id, const GroupCardLookup& cards, const int64_t now_ms) {
  auto change = DraftOn(roster, group_id, local_identity, local_device, GroupChangeKind::LinkRevoke, now_ms);
  if (!change) {
    return change.error();
  }
  auto metadata = MetadataOf(roster, group_id);
  if (!metadata) {
    return metadata.error();
  }
  const std::optional<GroupRosterView> view = CurrentView(roster, group_id, cards, now_ms);
  const GroupInviteLink* link = view ? view->LiveLink(link_id) : nullptr;
  if (!link) {
    return Error(kGroupLinkGoneError);
  }
  const std::optional<MemberRole> role = OwnRole(roster, *metadata, local_identity);
  const bool own = link->maker == local_identity;
  if (!role || (!own && !GroupAllows(GroupAction::RevokeInviteLink, *role, metadata->policy.preset))) {
    return Error(kGroupLinkRevokeNotAllowedError);
  }
  change->link_id = link_id;
  return change;
}

Roe<GroupChange> DraftRename(IGroupRosterStore& roster, const std::string& group_id, const std::string& local_identity,
                             const std::string& local_device, const std::string& title, const GroupCardLookup& cards,
                             const int64_t now_ms) {
  auto change = DraftOn(roster, group_id, local_identity, local_device, GroupChangeKind::Rename, now_ms);
  if (!change) {
    return change.error();
  }
  if (title.empty() || title.size() > group_change::kMaxTitleBytes) {
    return Error("Group title required");
  }
  auto metadata = MetadataOf(roster, group_id);
  if (!metadata) {
    return metadata.error();
  }
  const std::optional<MemberRole> role = OwnRole(roster, *metadata, local_identity);
  if (!role || !GroupAllows(GroupAction::Rename, *role, metadata->policy.preset)) {
    return Error(kGroupRenameNotAllowedError);
  }
  uint64_t version = metadata->title_version;
  if (const std::optional<GroupRosterView> view = CurrentView(roster, group_id, cards, now_ms)) {
    version = std::max(version, view->title_version);
  }
  change->title = title;
  change->title_version = version + 1;
  return change;
}

std::vector<std::string> RemovableAddedBy(IGroupRosterStore& roster, const std::string& group_id,
                                          const std::string& local_identity, const std::string& account) {
  std::vector<std::string> targets;
  auto metadata = roster.LoadMetadata(group_id);
  auto members = roster.ListMembers(group_id);
  if (!metadata || !*metadata || !members || account.empty()) {
    return targets;
  }
  const std::optional<MemberRole> role = OwnRole(roster, **metadata, local_identity);
  if (!role) {
    return targets;
  }
  for (const GroupRosterMember& m : *members) {
    const MemberRole target = m.member_identity == (*metadata)->owner_identity ? MemberRole::Owner : m.role;
    if (m.added_by == account && m.member_identity != account && m.member_identity != local_identity &&
        GroupAllows(GroupAction::Remove, *role, (*metadata)->policy.preset, target)) {
      targets.push_back(m.member_identity); // an admin someone added stays: only the owner removes admins
    }
  }
  std::sort(targets.begin(), targets.end());
  return targets;
}

std::vector<std::string> RemovedBy(IGroupRosterStore& roster, const std::string& group_id, const std::string& account,
                                   const GroupCardLookup& cards, const int64_t now_ms) {
  std::vector<std::string> removed;
  const std::optional<GroupRosterView> view = CurrentView(roster, group_id, cards, now_ms);
  if (!view || account.empty()) {
    return removed;
  }
  for (const GroupRemovedMember& r : view->removed) {
    if (r.removed_by == account && !view->Lists(r.identity)) {
      removed.push_back(r.identity);
    }
  }
  return removed;
}

GroupRosterView FoldBase(IGroupRosterStore& roster, const GroupMetadata& metadata,
                         const std::vector<GroupRosterStatementMember>& rows, const GroupCardLookup& cards,
                         const int64_t now_ms, GroupRosterStatement& base) {
  if (auto held = Held(metadata)) {
    base = std::move(*held);
  } else {
    base = GroupRosterStatement{};
    base.group_id = metadata.group_id;
    base.members = rows;
    base.preset = metadata.policy.preset;
    base.owner_identity = metadata.owner_identity;
  }
  return ComputeRosterChecked(base, CheckChanges(HeldChanges(roster, metadata.group_id), cards, now_ms), now_ms);
}

GroupRosterStatement OwnerDraft(IGroupRosterStore& roster, const GroupMetadata& metadata,
                                std::vector<GroupRosterStatementMember> rows, std::vector<GroupOwnerTransfer> chain,
                                const std::string& owner_identity, const std::string& owner_key_b64,
                                const GroupCardLookup& cards, const int64_t now_ms) {
  GroupRosterStatement base;
  const GroupRosterView view = FoldBase(roster, metadata, rows, cards, now_ms, base);
  GroupRosterStatement draft;
  draft.group_id = metadata.group_id;
  draft.roster_epoch = std::max(metadata.roster_epoch, base.roster_epoch);
  draft.members = std::move(rows);
  draft.owner_chain = std::move(chain);
  draft.preset = metadata.policy.preset;
  draft.title = metadata.title;
  draft.title_version = metadata.title_version;
  for (const GroupRemovedMember& r : view.removed) {
    if (!draft.Lists(r.identity)) {
      draft.removed.push_back(r); // one the owner brought back is a member, not removed
    }
  }
  for (const GroupInviteLink& l : view.links) {
    if (draft.Lists(l.maker)) {
      draft.links.push_back(l);
    }
  }
  draft.owner_identity = owner_identity;
  draft.owner_public_key_b64 = owner_key_b64;
  FoldInto(base, view, draft);
  const bool same_epoch = !base.owner_signature_b64.empty() && draft.roster_epoch == base.roster_epoch;
  if (same_epoch && group_roster_statement::SignBytes(draft) != group_roster_statement::SignBytes(base)) {
    draft.roster_epoch += 1; // it says something new: never two statements at one epoch
    FoldInto(base, view, draft);
  }
  return draft;
}

bool HoldsFoldable(IGroupRosterStore& roster, const std::string& group_id, const GroupCardLookup& cards,
                   const int64_t now_ms) {
  auto metadata = roster.LoadMetadata(group_id);
  if (!metadata || !*metadata || (*metadata)->sync_state != GroupSyncState::Active) {
    return false;
  }
  std::vector<GroupRosterStatementMember> rows;
  if (auto members = roster.ListMembers(group_id)) {
    for (const GroupRosterMember& m : *members) {
      rows.push_back({.identity = m.member_identity, .role = m.role, .joined_at = m.joined_at, .added_by = m.added_by});
    }
  }
  GroupRosterStatement base;
  const GroupRosterView view = FoldBase(roster, **metadata, rows, cards, now_ms, base);
  return std::any_of(view.verdicts.begin(), view.verdicts.end(),
                     [](const GroupChangeVerdict& v) { return FoldsIn(v); });
}

std::vector<std::string> GroupsWithChangesBy(IGroupRosterStore& roster, const std::string& signer) {
  std::vector<std::string> groups;
  auto ids = roster.ListGroupsWithChanges();
  if (!ids) {
    return groups;
  }
  for (const std::string& group_id : *ids) {
    const std::vector<GroupChange> held = HeldChanges(roster, group_id);
    if (std::any_of(held.begin(), held.end(), [&](const GroupChange& c) { return c.signer == signer; })) {
      groups.push_back(group_id);
    }
  }
  return groups;
}

GroupCopyState CopyState(IGroupRosterStore& roster, const std::string& group_id) {
  GroupCopyState state;
  auto metadata = roster.LoadMetadata(group_id);
  if (metadata && *metadata) {
    const std::optional<GroupRosterStatement> held = Held(**metadata);
    state.statement_epoch = held ? held->roster_epoch : 0;
  }
  std::vector<std::string> ids;
  if (auto stored = roster.ListGroupChanges(group_id)) {
    for (const StoredGroupChange& row : *stored) {
      ids.push_back(row.change_id);
    }
  }
  state.changes_hash = group_change::ChangesHash(std::move(ids));
  return state;
}

bool NeedsCatchUp(const GroupCopyState& ours, const GroupCopyState& theirs) {
  if (theirs.statement_epoch != ours.statement_epoch) {
    return theirs.statement_epoch > ours.statement_epoch;
  }
  return !theirs.changes_hash.empty() && theirs.changes_hash != ours.changes_hash;
}

bool CatchUpLimiter::Allow(const std::string& group_id, const int64_t now_ms) {
  std::lock_guard lock(mutex_);
  auto [slot, fresh] = last_.emplace(group_id, now_ms);
  if (fresh) {
    return true;
  }
  if (now_ms - slot->second < kCatchUpGapMs) {
    return false;
  }
  slot->second = now_ms;
  return true;
}

Roe<std::optional<group_roster_sync_codec::RosterRequest>> BeginCatchUp(IGroupRosterStore& roster,
                                                                        const std::string& group_id,
                                                                        const int64_t now_ms,
                                                                        const std::string& after) {
  auto lock = IGroupRosterStore::LockForUpdate();
  auto metadata = MetadataOf(roster, group_id);
  if (!metadata) {
    return metadata.error();
  }
  if (metadata->sync_state != GroupSyncState::Active) {
    return std::optional<group_roster_sync_codec::RosterRequest>{};
  }
  const GroupCopyState state = CopyState(roster, group_id);
  group_roster_sync_codec::RosterRequest request{.group_id = group_id,
                                                 .request_id = util::GenerateUuid(),
                                                 .roster_epoch = metadata->roster_epoch,
                                                 .statement_epoch = state.statement_epoch,
                                                 .changes_hash = state.changes_hash,
                                                 .after = after};
  metadata->sync_request_id = request.request_id;
  metadata->sync_requested_at = now_ms;
  if (auto saved = roster.UpsertMetadata(*metadata); !saved) {
    return saved.error();
  }
  return std::optional<group_roster_sync_codec::RosterRequest>(std::move(request));
}

void AddChangesTo(IGroupRosterStore& roster, const group_roster_sync_codec::RosterRequest& request,
                  group_roster_sync_codec::RosterReply& reply) {
  if (CopyState(roster, request.group_id).changes_hash == request.changes_hash) {
    return; // the asker holds what we hold
  }
  auto stored = roster.ListGroupChanges(request.group_id);
  if (!stored) {
    return;
  }
  std::sort(stored->begin(), stored->end(),
            [](const StoredGroupChange& a, const StoredGroupChange& b) { return a.change_id < b.change_id; });
  std::set<std::string> carded;
  size_t bytes = 0;
  for (const StoredGroupChange& row : *stored) {
    if (!request.after.empty() && row.change_id <= request.after) {
      continue;
    }
    auto change = ParseChange(row);
    if (!change) {
      continue;
    }
    if (change->signer_card && !carded.insert(change->signer).second) {
      change->signer_card.reset(); // the signer's card rides this reply once
    }
    const size_t size = DumpJson(group_change::ToJson(*change)).size();
    if (reply.changes.size() >= group_roster_sync_codec::kMaxReplyChanges ||
        (!reply.changes.empty() && bytes + size > group_roster_sync_codec::kReplyChangesBudgetBytes)) {
      reply.more = true;
      return;
    }
    bytes += size;
    reply.changes.push_back(std::move(*change));
  }
}

group_roster_sync_codec::CarriedChanges ChangesForNewcomer(IGroupRosterStore& roster, const std::string& group_id,
                                                           const std::string& first) {
  group_roster_sync_codec::CarriedChanges carried;
  auto stored = roster.ListGroupChanges(group_id);
  if (!stored) {
    return carried;
  }
  std::sort(stored->begin(), stored->end(), [&first](const StoredGroupChange& a, const StoredGroupChange& b) {
    if ((a.change_id == first) != (b.change_id == first)) {
      return a.change_id == first;
    }
    return a.change_id < b.change_id;
  });
  std::set<std::string> carded;
  size_t bytes = 0;
  for (const StoredGroupChange& row : *stored) {
    auto change = ParseChange(row);
    if (!change) {
      continue;
    }
    if (change->signer_card && !carded.insert(change->signer).second) {
      change->signer_card.reset(); // the signer's card rides the bundle once
    }
    const size_t size = DumpJson(group_change::ToJson(*change)).size();
    if (carried.changes.size() >= group_roster_sync_codec::kMaxReplyChanges ||
        (!carried.changes.empty() && bytes + size > group_roster_sync_codec::kReplyChangesBudgetBytes)) {
      carried.more = true;
      break;
    }
    bytes += size;
    carried.changes.push_back(std::move(*change));
  }
  return carried;
}

Roe<bool> KeepStatement(IGroupRosterStore& roster, const GroupRosterStatement& statement) {
  if (statement.dissolved) {
    return Error("roster statement: a dissolve is not a roster"); // G016: never held as the group's roster
  }
  auto lock = IGroupRosterStore::LockForUpdate();
  auto metadata = MetadataOf(roster, statement.group_id);
  if (!metadata) {
    return metadata.error();
  }
  if (metadata->sync_state == GroupSyncState::Dissolved) {
    return false; // an ended group keeps no roster
  }
  if (auto valid = group_roster_statement::Verify(statement, metadata->owner_identity); !valid) {
    return valid.error();
  }
  if (!CandidateWins(Held(*metadata), statement)) {
    return false;
  }
  metadata->roster_statement_json = ToJsonText(statement);
  metadata->policy.preset = statement.preset; // the owner's latest word, also for a newcomer (G017)
  if (auto saved = roster.UpsertMetadata(*metadata); !saved) {
    return saved.error();
  }
  return true;
}

Roe<bool> KeepAndCatchUp(IGroupRosterStore& roster, const GroupRosterStatement& statement, const GroupCardLookup& cards,
                         const int64_t now_ms, GroupViewApplied* applied) {
  auto lock = IGroupRosterStore::LockForUpdate();
  auto kept = KeepStatement(roster, statement);
  if (!kept || !*kept) {
    return kept;
  }
  auto view = ApplyView(roster, statement.group_id, cards, now_ms);
  if (!view) {
    return view.error();
  }
  if (applied) {
    *applied = std::move(*view);
  }
  return true;
}

Roe<Check> BeginCheck(IGroupRosterStore& roster, const std::string& group_id, const std::string& local_identity,
                      const int64_t now_ms) {
  auto lock = IGroupRosterStore::LockForUpdate();
  auto metadata = MetadataOf(roster, group_id);
  if (!metadata) {
    return metadata.error();
  }
  auto members = roster.ListMembers(group_id);
  if (!members) {
    return members.error();
  }
  Check check;
  check.request.group_id = group_id;
  check.request.request_id = util::GenerateUuid();
  check.request.roster_epoch = metadata->roster_epoch;
  const GroupCopyState state = CopyState(roster, group_id);
  check.request.statement_epoch = state.statement_epoch;
  check.request.changes_hash = state.changes_hash;
  std::set<std::string> ask;
  if (!metadata->owner_identity.empty()) {
    ask.insert(metadata->owner_identity);
  }
  for (const GroupRosterMember& m : *members) {
    ask.insert(m.member_identity);
  }
  ask.erase(local_identity);
  check.ask.assign(ask.begin(), ask.end());

  if (metadata->sync_state == GroupSyncState::Dissolved) {
    check.ask.clear();  // G016: an ended group checks nothing and asks no one
    return check;
  }
  if (metadata->sync_state != GroupSyncState::Removed) {
    metadata->sync_state = GroupSyncState::Checking;
  }
  metadata->sync_request_id = check.request.request_id;
  metadata->sync_requested_at = now_ms;
  if (auto saved = roster.UpsertMetadata(*metadata); !saved) {
    return saved.error();
  }
  return check;
}

Roe<std::optional<group_roster_sync_codec::RosterReply>> Answer(IGroupRosterStore& roster,
                                                                const group_roster_sync_codec::RosterRequest& request,
                                                                const std::string& asker) {
  auto metadata = roster.LoadMetadata(request.group_id);
  if (!metadata) {
    return metadata.error();
  }
  if (!metadata->has_value()) {
    return std::optional<group_roster_sync_codec::RosterReply>{};
  }
  group_roster_sync_codec::RosterReply reply;
  reply.group_id = request.group_id;
  reply.request_id = request.request_id;
  reply.statement = Held(**metadata);  // for a dissolved group (G016): the owner's dissolve, which ends the asker's check
  if ((*metadata)->sync_state == GroupSyncState::Active) {
    auto member = roster.IsMember(request.group_id, asker);
    if (!member) {
      return member.error();
    }
    reply.removed = !*member;
    AddChangesTo(roster, request, reply);
  }
  return std::optional<group_roster_sync_codec::RosterReply>(std::move(reply));
}

namespace {

/** The reply's statement kept (it verified), its changes held, the roster recomputed; whether it lists us. */
Roe<bool> TakeReply(IGroupRosterStore& roster, const group_roster_sync_codec::RosterReply& reply,
                    const std::string& local_identity, const GroupCardLookup& cards, const int64_t now_ms,
                    GroupViewApplied* applied) {
  if (auto kept = KeepStatement(roster, *reply.statement); !kept) {
    return kept.error();
  }
  if (auto held = HoldCarried(roster, reply.changes, cards, now_ms); !held) {
    return held.error();
  }
  auto view = ApplyView(roster, reply.group_id, cards, now_ms);
  if (!view) {
    return view.error();
  }
  bool listed = true;
  if (view->view) {
    listed = view->view->Lists(local_identity);
  } else if (auto metadata = MetadataOf(roster, reply.group_id)) {
    const std::optional<GroupRosterStatement> held = Held(*metadata);
    listed = held ? held->Lists(local_identity) : true;
  }
  if (applied) {
    *applied = std::move(*view);
  }
  return listed;
}

} // namespace

Roe<ReplyOutcome> ApplyReply(IGroupRosterStore& roster, const group_roster_sync_codec::RosterReply& reply,
                             const std::string& local_identity, const std::string& replier,
                             const GroupCardLookup& cards, const int64_t now_ms, GroupDissolvedApplied* dissolved,
                             GroupViewApplied* applied) {
  auto lock = IGroupRosterStore::LockForUpdate();
  auto metadata = MetadataOf(roster, reply.group_id);
  if (!metadata) {
    return metadata.error();
  }
  if (metadata->sync_state == GroupSyncState::Dissolved || metadata->sync_request_id.empty() ||
      reply.request_id != metadata->sync_request_id) {
    return ReplyOutcome::Ignored;
  }
  const auto settle = [&](GroupSyncState state) -> Roe<ReplyOutcome> {
    auto now = MetadataOf(roster, reply.group_id);
    if (!now) {
      return now.error();
    }
    now->sync_state = state;
    if (auto saved = roster.UpsertMetadata(*now); !saved) {
      return saved.error();
    }
    return state == GroupSyncState::Removed ? ReplyOutcome::Removed : ReplyOutcome::Active;
  };
  if (!reply.statement) {
    // Without a statement only the owner can tell us we are out.
    if (reply.removed && replier == metadata->owner_identity) {
      return settle(GroupSyncState::Removed);
    }
    return ReplyOutcome::Ignored;
  }
  const GroupRosterStatement& statement = *reply.statement;
  if (statement.dissolved) {
    // G016: the owner ended the group (a member relays its dissolve, or the owner answers with it). Authority as for
    // any dissolve; one that does not hold tells us nothing.
    auto ended = ApplyDissolvedStatement(roster, statement, local_identity);
    if (!ended || !ended->applied) {
      return ReplyOutcome::Ignored;
    }
    if (dissolved) {
      *dissolved = std::move(*ended);
    }
    return ReplyOutcome::Dissolved;
  }
  if (auto valid = group_roster_statement::Verify(statement, metadata->owner_identity); !valid) {
    return ReplyOutcome::Ignored;
  }
  auto listed = TakeReply(roster, reply, local_identity, cards, now_ms, applied);
  if (!listed) {
    return listed.error();
  }
  return settle(*listed ? GroupSyncState::Active : GroupSyncState::Removed);
}

bool StillAwaiting(const GroupMetadata& metadata, const std::string& request_id) {
  return metadata.sync_state == GroupSyncState::Checking && !request_id.empty() &&
         metadata.sync_request_id == request_id;
}

bool MaySend(const GroupMetadata& metadata) {
  return metadata.sync_state == GroupSyncState::Active;
}

} // namespace group_roster_sync
} // namespace pbr
