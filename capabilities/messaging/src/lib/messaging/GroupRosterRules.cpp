#include "lib/messaging/GroupRosterRules.h"

#include "lib/messaging/GroupPermissions.h"

#include <algorithm>
#include <map>
#include <set>
#include <tuple>
#include "common/PbrCompat.h"

namespace pbr {
namespace {

using Reason = GroupChangeReason;
using Status = GroupChangeStatus;
using Proof = GroupChangeProof;
using Kind = GroupChangeKind;

// ---- Checking signatures -------------------------------------------------------------------------------------

/** Newest first: higher issued_at, then the lower signature (a fixed order for equal dates). */
bool NewerCard(const AccountCard& a, const AccountCard& b) {
  return std::tie(b.issued_at, a.card_signature_b64) < std::tie(a.issued_at, b.card_signature_b64);
}

/** Every card that proves itself for `account`: the held one and those the changes carry, newest first. */
std::vector<AccountCard> ProvedCards(const std::string& account, const std::vector<GroupChange>& changes,
                                     const GroupCardLookup& cards, const int64_t now_ms) {
  std::vector<AccountCard> candidates;
  if (cards) {
    if (auto held = cards(account)) {
      candidates.push_back(std::move(*held));
    }
  }
  for (const GroupChange& c : changes) {
    if (c.signer == account && c.signer_card) {
      candidates.push_back(*c.signer_card);
    }
  }
  std::vector<AccountCard> proved;
  for (AccountCard& card : candidates) {
    const bool seen = std::find(proved.begin(), proved.end(), card) != proved.end();
    if (!seen && card.keys.account_id == account && VerifyAccountCard(card, now_ms)) {
      proved.push_back(std::move(card));
    }
  }
  std::sort(proved.begin(), proved.end(), NewerCard);
  return proved;
}

CheckedGroupChange CheckOne(const GroupChange& change, const std::vector<AccountCard>& cards) {
  CheckedGroupChange checked{.change = change, .id = group_change::Id(change)};
  if (!group_change::CheckShape(change)) {
    checked.proof = Proof::Malformed;
    return checked;
  }
  for (const AccountCard& card : cards) {
    if (const AccountCardDevice* device = FindCardDevice(card, change.signer_device)) {
      checked.proof = group_change::VerifyDeviceSignature(change, *device) ? Proof::Proved : Proof::BadSignature;
      checked.device_current = &card == &cards.front();
      return checked;
    }
  }
  checked.proof = Proof::NoCard;
  return checked;
}

/** Of two copies of one change, the one to keep: proved, then missing a card, then the lower signature. */
bool BetterCopy(const CheckedGroupChange& a, const CheckedGroupChange& b) {
  const auto rank = [](const CheckedGroupChange& c) {
    return c.proof == Proof::Proved ? 0 : c.proof == Proof::NoCard ? 1 : 2;
  };
  if (rank(a) != rank(b)) {
    return rank(a) < rank(b);
  }
  return a.change.signature_b64 < b.change.signature_b64;
}

std::vector<CheckedGroupChange> Collapse(const std::vector<CheckedGroupChange>& changes) {
  std::map<std::string, const CheckedGroupChange*> best;
  for (const CheckedGroupChange& c : changes) {
    auto [slot, fresh] = best.emplace(c.id, &c);
    if (!fresh && BetterCopy(c, *slot->second)) {
      slot->second = &c;
    }
  }
  std::vector<CheckedGroupChange> out;
  out.reserve(best.size());
  for (const auto& [id, c] : best) {
    out.push_back(*c);
  }
  return out;
}

// ---- The rules -----------------------------------------------------------------------------------------------

int RoleRank(const MemberRole role) {
  return role == MemberRole::Owner ? 2 : role == MemberRole::Admin ? 1 : 0;
}

bool Names(const std::vector<std::string>& ids, const std::string& id) {
  return std::find(ids.begin(), ids.end(), id) != ids.end();
}

/** Where a signer stands, as one rule sees it. */
struct SignerState {
  /** In the group now. */
  bool in = false;
  /** Came in and was not removed (a leaver is present, not in). */
  bool present = false;
  MemberRole role_now = MemberRole::Member;
  /** The role a leaver's kept changes are judged at. */
  MemberRole role_before_leave = MemberRole::Member;
};

using Changes = std::vector<const CheckedGroupChange*>;

/**
 * One run of R1–R6 over a statement and a set of checked changes. Every loop walks maps keyed by change id or
 * account, so the result depends on the set alone. Removals come first (only the statement's owner and admins
 * may remove, so they do not depend on who the adds bring in), then the adds to a fixed point, then renames and
 * links over the members that leaves.
 */
class RosterRules {
public:
  RosterRules(const GroupRosterStatement& statement, const int64_t now_ms) : s_(statement), now_(now_ms) {
    for (const GroupRosterStatementMember& m : s_.members) {
      s_members_[m.identity] = &m;
    }
    for (const GroupRemovedMember& r : s_.removed) {
      s_removed_[r.identity] = &r;
    }
  }

  GroupRosterView Run(const std::vector<CheckedGroupChange>& changes) {
    Classify(changes);
    if (s_.dissolved) {
      return Build();
    }
    Index();
    RunAdds();
    JudgeAdds();
    JudgeRemovesAndLeaves();
    JudgeRenames();
    JudgeLinks();
    return Build();
  }

private:
  // ---- Sorting the changes in ----

  void Mark(const CheckedGroupChange& c, const Status status, const Reason reason) {
    verdicts_[c.id] = {
        .id = c.id, .kind = c.change.kind, .signer = c.change.signer, .status = status, .reason = reason};
  }

  /** Drop what is not this group's live business; hold the rest (unproved ones as void, for later). */
  void Classify(const std::vector<CheckedGroupChange>& changes) {
    const std::set<std::string> folded(s_.folded.begin(), s_.folded.end());
    for (const CheckedGroupChange& c : changes) {
      if (c.change.group_id != s_.group_id) {
        Mark(c, Status::Dropped, Reason::OtherGroup);
      } else if (folded.count(c.id) > 0) {
        Mark(c, Status::Dropped, Reason::Folded);
      } else if (s_.dissolved) {
        Mark(c, Status::Dropped, Reason::Dissolved);
      } else if (c.proof == Proof::Malformed) {
        Mark(c, Status::Dropped, Reason::Malformed);
      } else if (c.proof == Proof::BadSignature) {
        Mark(c, Status::Dropped, Reason::BadSignature);
      } else if (c.proof == Proof::NoCard) {
        Mark(c, Status::Void, Reason::NoCard);
        joining_.insert(c.change.kind == Kind::Add ? c.change.member : std::string());
      } else {
        held_[c.id] = &c;
      }
    }
    joining_.erase(std::string());
  }

  void Index() {
    for (const auto& [id, c] : held_) {
      const GroupChange& ch = c->change;
      switch (ch.kind) {
        case Kind::Add:
          adds_.push_back(c);
          joining_.insert(ch.member);
          break;
        case Kind::Remove:
          removes_of_[ch.member].push_back(c);
          for (const std::string& kept : ch.keeps) {
            kept_by_remove_[kept].push_back(c);
          }
          break;
        case Kind::Leave:
          leaves_by_[ch.signer].push_back(c);
          for (const std::string& kept : ch.keeps) {
            kept_by_leave_[kept].push_back(c);
          }
          break;
        case Kind::Rename:
          renames_.push_back(c);
          break;
        case Kind::LinkAdd:
          link_adds_.push_back(c);
          break;
        case Kind::LinkRevoke:
          link_revokes_.push_back(c);
          break;
      }
    }
  }

  // ---- Memoized facts (removals, leaves, keeps) ----

  /** `compute` once per key; a key met again while computing it is false (the change ids form no cycle). */
  template <typename F>
  bool Memo(std::map<std::string, bool>& memo, const std::string& key, F compute) {
    if (auto found = memo.find(key); found != memo.end()) {
      return found->second;
    }
    const std::pair<const void*, std::string> visit{&memo, key};
    if (!visiting_.insert(visit).second) {
      return false;
    }
    const bool value = compute();
    visiting_.erase(visit);
    memo[key] = value;
    return value;
  }

  static const Changes& Of(const std::map<std::string, Changes>& index, const std::string& key) {
    static const Changes kNone;
    auto found = index.find(key);
    return found == index.end() ? kNone : found->second;
  }

  MemberRole StatementRole(const std::string& account) const {
    auto found = s_members_.find(account);
    return found == s_members_.end() ? MemberRole::Member : found->second->role;
  }

  bool InStatement(const std::string& account) const { return s_members_.count(account) > 0; }

  /** R3: a `remove` of the signer, itself valid, lists `c` in its keeps (the remover vouches for it). */
  bool RemoveKept(const CheckedGroupChange& c) {
    return Memo(remove_kept_memo_, c.id, [&] {
      auto found = kept_by_remove_.find(c.id);
      if (found == kept_by_remove_.end()) {
        return false;
      }
      return std::any_of(found->second.begin(), found->second.end(), [&](const CheckedGroupChange* r) {
        return r->change.member == c.change.signer && ValidRemove(*r);
      });
    });
  }

  /** R3: the signer's own valid `leave` lists `c` (and nobody removed the signer: then the remover decides). */
  bool LeaveKept(const CheckedGroupChange& c) {
    return Memo(leave_kept_memo_, c.id, [&] {
      auto found = kept_by_leave_.find(c.id);
      if (found == kept_by_leave_.end() || RemovedByChange(c.change.signer)) {
        return false;
      }
      return std::any_of(found->second.begin(), found->second.end(), [&](const CheckedGroupChange* l) {
        return l->change.signer == c.change.signer && ValidLeave(*l);
      });
    });
  }

  bool ValidRemove(const CheckedGroupChange& r) {
    return Memo(removes_memo_, r.id, [&] { return RemoveReason(r) == Reason::None; });
  }

  bool ValidLeave(const CheckedGroupChange& l) {
    return Memo(leaves_memo_, l.id, [&] { return LeaveReason(l) == Reason::None; });
  }

  bool RemovedByChange(const std::string& account) {
    return Memo(removed_memo_, account, [&] {
      auto found = removes_of_.find(account);
      return found != removes_of_.end() && std::any_of(found->second.begin(), found->second.end(),
                                                       [&](const CheckedGroupChange* r) { return ValidRemove(*r); });
    });
  }

  bool LeftByChange(const std::string& account) {
    auto found = leaves_by_.find(account);
    return found != leaves_by_.end() && std::any_of(found->second.begin(), found->second.end(),
                                                    [&](const CheckedGroupChange* l) { return ValidLeave(*l); });
  }

  /** The statement's membership of `account` stands: listed, not removed, not left. */
  bool StatementStands(const std::string& account) {
    return InStatement(account) && !RemovedByChange(account) && !LeftByChange(account);
  }

  SignerState FromStatement(const std::string& account) {
    SignerState st;
    st.present = InStatement(account) && !RemovedByChange(account);
    st.in = st.present && !LeftByChange(account);
    st.role_now = st.in ? StatementRole(account) : MemberRole::Member;
    st.role_before_leave = st.present ? StatementRole(account) : MemberRole::Member;
    return st;
  }

  SignerState FromRoster(const std::string& account) {
    SignerState st = FromStatement(account);
    st.in = in_.count(account) > 0;
    st.present = present_.count(account) > 0;
    return st;
  }

  /** Not in the group: for good (left, removed, known to the statement) or not yet (no add of them stands). */
  Reason NotInReason(const std::string& account) {
    const bool known =
        InStatement(account) || s_removed_.count(account) > 0 || RemovedByChange(account) || LeftByChange(account);
    return known ? Reason::SignerOut : Reason::SignerNotJoined;
  }

  /**
   * R3 and R6 for `c`: kept by a remove of its signer (counts outright); else its device on the newest card
   * (or the signer's leave kept it), its signer in the group (or present and leave-kept), and the action allowed
   * at the signer's role under the latest statement. `role` gets the role it was judged at.
   */
  template <typename State>
  Reason Entitlement(const CheckedGroupChange& c, const GroupAction action, const MemberRole target, State state,
                     MemberRole* role = nullptr) {
    const std::string& signer = c.change.signer;
    MemberRole acting = StatementRole(signer);
    if (RemoveKept(c)) {
      if (role) {
        *role = acting;
      }
      return Reason::None;
    }
    // The statement's role is the most the signer can hold: a cheap refusal before any recursion.
    if (!GroupAllows(action, acting, s_.preset, target)) {
      return Reason::NotAllowed;
    }
    const bool leave_kept = LeaveKept(c);
    if (!c.device_current && !leave_kept) {
      return Reason::DeviceOffCard;
    }
    const SignerState st = state(signer);
    if (st.in) {
      acting = st.role_now;
    } else if (leave_kept && st.present) {
      acting = st.role_before_leave;
    } else {
      return NotInReason(signer);
    }
    if (!GroupAllows(action, acting, s_.preset, target)) {
      return Reason::NotAllowed;
    }
    if (role) {
      *role = acting;
    }
    return Reason::None;
  }

  Reason RemoveReason(const CheckedGroupChange& r) {
    if (r.change.member == s_.owner_identity) {
      return Reason::NotAllowed;
    }
    return Entitlement(r, GroupAction::Remove, StatementRole(r.change.member),
                       [this](const std::string& a) { return FromStatement(a); });
  }

  /** A leave needs no standing beyond being the signer's own: anyone but the owner, from a device on the card. */
  Reason LeaveReason(const CheckedGroupChange& l) {
    if (!GroupAllows(GroupAction::Leave, StatementRole(l.change.signer), s_.preset)) {
      return Reason::NotAllowed;
    }
    if (!l.device_current && !RemoveKept(l)) {
      return Reason::DeviceOffCard;
    }
    return Reason::None;
  }

  // ---- Adds (R1, R2, R5) ----

  /** R1 / R2 reasons an add fails or is undone, before who signed it matters. */
  Reason StaticAddReason(const CheckedGroupChange& a) {
    const GroupChange& ch = a.change;
    const Changes& removes = Of(removes_of_, ch.member);
    for (const CheckedGroupChange* r : removes) {
      if (ValidRemove(*r) && Names(r->change.adds, a.id)) {
        return Reason::NamedByRemove;
      }
    }
    if (RemovedByChange(ch.member) && StatementRole(ch.signer) == MemberRole::Member) {
      return Reason::RemovedMember;
    }
    if (auto removed = s_removed_.find(ch.member); removed != s_removed_.end()) {
      if (ch.readmits.empty() || ch.readmits != removed->second->removal) {
        return Reason::NotReadmitted;
      }
    } else if (!ch.readmits.empty()) {
      const bool names_removal = std::any_of(removes.begin(), removes.end(), [&](const CheckedGroupChange* r) {
        return r->id == ch.readmits && ValidRemove(*r);
      });
      if (!names_removal) {
        return Reason::NotReadmitted;
      }
    }
    for (const CheckedGroupChange* l : Of(leaves_by_, ch.member)) {
      if (ValidLeave(*l) && Names(l->change.adds, a.id)) {
        return Reason::NamedByLeave;
      }
    }
    return Reason::None;
  }

  Reason AddEntitlement(const CheckedGroupChange& a) {
    const GroupAction action = a.change.readmits.empty() ? GroupAction::Invite : GroupAction::Readmit;
    return Entitlement(a, action, MemberRole::Member, [this](const std::string& who) { return FromRoster(who); });
  }

  /** Take `a` in if its signer stands now; an add a leave names makes its member present only (no place). */
  bool TryAccept(const CheckedGroupChange& a) {
    if (AddEntitlement(a) != Reason::None) {
      return false;
    }
    const std::string& member = a.change.member;
    if (static_reasons_[a.id] == Reason::NamedByLeave) {
      present_.insert(member);
    } else if (in_.count(member) == 0) {
      if (in_.size() >= kMaxGroupMembers) {
        return false; // R5
      }
      in_.insert(member);
      present_.insert(member);
      joined_by_[member] = &a;
    }
    accepted_.insert(a.id);
    return true;
  }

  /**
   * The statement's members that stand, then the adds in change-id order, over and over until none more is taken
   * in: an add counts once its signer is in, and takes a place while fewer than 50 are in (R5).
   */
  void RunAdds() {
    for (const auto& [identity, m] : s_members_) {
      if (!RemovedByChange(identity)) {
        present_.insert(identity);
      }
      if (StatementStands(identity)) {
        in_.insert(identity);
      }
    }
    std::vector<const CheckedGroupChange*> open;
    for (const CheckedGroupChange* a : adds_) {
      const Reason reason = StaticAddReason(*a);
      static_reasons_[a->id] = reason;
      if (reason == Reason::None || reason == Reason::NamedByLeave) {
        open.push_back(a);
      }
    }
    for (bool taken = true; taken;) {
      taken = false;
      for (const CheckedGroupChange* a : open) {
        if (accepted_.count(a->id) == 0 && TryAccept(*a)) {
          taken = true;
        }
      }
    }
  }

  void JudgeAdds() {
    for (const CheckedGroupChange* a : adds_) {
      const Reason static_reason = static_reasons_[a->id];
      if (accepted_.count(a->id) > 0) {
        Mark(*a, static_reason == Reason::NamedByLeave ? Status::Undone : Status::Applied,
             static_reason == Reason::NamedByLeave ? Reason::NamedByLeave : Reason::None);
      } else if (static_reason == Reason::NamedByRemove) {
        Mark(*a, Status::Undone, Reason::NamedByRemove);
      } else if (static_reason != Reason::None && static_reason != Reason::NamedByLeave) {
        Mark(*a, Status::Void, static_reason);
      } else {
        const Reason reason = AddEntitlement(*a);
        Mark(*a, Status::Void, reason == Reason::None ? Reason::GroupFull : reason);
      }
    }
  }

  // ---- Removes, leaves, renames, links ----

  void JudgeRemovesAndLeaves() {
    for (const auto& [member, removes] : removes_of_) {
      for (const CheckedGroupChange* r : removes) {
        const Reason reason = ValidRemove(*r) ? Reason::None : RemoveReason(*r);
        Mark(*r, reason == Reason::None ? Status::Applied : Status::Void, reason);
      }
    }
    for (const auto& [signer, leaves] : leaves_by_) {
      // A leave by someone neither the statement nor any add knows may matter once their add arrives.
      const bool known = InStatement(signer) || s_removed_.count(signer) > 0 || joining_.count(signer) > 0;
      for (const CheckedGroupChange* l : leaves) {
        Reason reason = ValidLeave(*l) ? Reason::None : LeaveReason(*l);
        if (reason == Reason::None && !known) {
          reason = Reason::SignerNotJoined;
        }
        Mark(*l, reason == Reason::None ? Status::Applied : Status::Void, reason);
      }
    }
  }

  /** R4: the highest title version, then the higher role, then the lower change id. */
  void JudgeRenames() {
    const CheckedGroupChange* best = nullptr;
    std::tuple<uint64_t, int> best_rank{0, 0};
    for (const CheckedGroupChange* c : renames_) {
      MemberRole role = MemberRole::Member;
      const Reason reason = Entitlement(
          *c, GroupAction::Rename, MemberRole::Member, [this](const std::string& who) { return FromRoster(who); },
          &role);
      if (reason != Reason::None) {
        Mark(*c, Status::Void, reason);
        continue;
      }
      if (c->change.title_version <= s_.title_version) {
        Mark(*c, Status::Superseded, Reason::OlderTitle);
        continue;
      }
      Mark(*c, Status::Superseded, Reason::LostRename);
      const std::tuple<uint64_t, int> rank{c->change.title_version, RoleRank(role)};
      if (!best || rank > best_rank) { // renames_ is in id order: on a tie the lower id stays
        best = c;
        best_rank = rank;
      }
    }
    title_ = s_.title;
    title_version_ = s_.title_version;
    if (best) {
      Mark(*best, Status::Applied, Reason::None);
      title_ = best->change.title;
      title_version_ = best->change.title_version;
    }
  }

  void JudgeLinkAdds() {
    for (const GroupInviteLink& link : s_.links) {
      links_[link.link_id] = link;
    }
    for (const CheckedGroupChange* c : link_adds_) {
      const Reason reason = Entitlement(*c, GroupAction::MakeInviteLink, MemberRole::Member,
                                        [this](const std::string& who) { return FromRoster(who); });
      if (reason != Reason::None) {
        Mark(*c, Status::Void, reason);
      } else if (links_.count(c->change.link_id) > 0) {
        Mark(*c, Status::Void, Reason::DuplicateLink);
      } else {
        Mark(*c, Status::Applied, Reason::None);
        const GroupChange& ch = c->change;
        links_[ch.link_id] = {.link_id = ch.link_id,
                              .secret_hash = ch.secret_hash,
                              .maker = ch.signer,
                              .expires_at = ch.expires_at,
                              .max_uses = ch.max_uses};
      }
    }
  }

  /** A link's maker revokes it, as do the owner and admins (G021). */
  void JudgeLinkRevokes() {
    for (const CheckedGroupChange* c : link_revokes_) {
      auto link = links_.find(c->change.link_id);
      if (link == links_.end()) {
        Mark(*c, Status::Void, Reason::NoSuchLink);
        continue;
      }
      const GroupAction action =
          c->change.signer == link->second.maker ? GroupAction::Send : GroupAction::RevokeInviteLink;
      const Reason reason =
          Entitlement(*c, action, MemberRole::Member, [this](const std::string& who) { return FromRoster(who); });
      Mark(*c, reason == Reason::None ? Status::Applied : Status::Void, reason);
      if (reason == Reason::None) {
        revoked_.insert(c->change.link_id);
      }
    }
  }

  void JudgeLinks() {
    JudgeLinkAdds();
    JudgeLinkRevokes();
  }

  // ---- The view ----

  GroupRosterViewMember MemberRow(const std::string& identity) {
    if (auto joined = joined_by_.find(identity); joined != joined_by_.end()) {
      const GroupChange& add = joined->second->change;
      return {.identity = identity,
              .role = MemberRole::Member,
              .joined_at = add.at,
              .added_by = add.signer,
              .added_in = joined->second->id};
    }
    const GroupRosterStatementMember& m = *s_members_.at(identity);
    return {.identity = identity, .role = m.role, .joined_at = m.joined_at, .added_by = m.added_by};
  }

  std::vector<GroupRemovedMember> RemovedRows() {
    std::map<std::string, GroupRemovedMember> removed;
    for (const auto& [identity, r] : s_removed_) {
      removed[identity] = *r;
    }
    for (const auto& [member, removes] : removes_of_) {
      for (const CheckedGroupChange* r : removes) { // in id order: the lowest valid one names the removal
        if (ValidRemove(*r)) {
          removed[member] = {.identity = member, .removal = r->id, .removed_by = r->change.signer};
          break;
        }
      }
    }
    std::vector<GroupRemovedMember> out;
    for (auto& [identity, row] : removed) {
      if (in_.count(identity) == 0) {
        out.push_back(std::move(row));
      }
    }
    return out;
  }

  GroupRosterView Build() {
    GroupRosterView view{.group_id = s_.group_id,
                         .statement_epoch = s_.roster_epoch,
                         .owner_identity = s_.owner_identity,
                         .preset = s_.preset,
                         .dissolved = s_.dissolved};
    view.title = s_.dissolved ? s_.title : title_;
    view.title_version = s_.dissolved ? s_.title_version : title_version_;
    if (!s_.dissolved) {
      for (const std::string& identity : in_) {
        view.members.push_back(MemberRow(identity));
      }
      view.removed = RemovedRows();
      for (const auto& [id, link] : links_) {
        if (revoked_.count(id) == 0 && in_.count(link.maker) > 0 && link.expires_at > now_) {
          view.links.push_back(link);
        }
      }
    }
    for (auto& [id, verdict] : verdicts_) {
      view.verdicts.push_back(std::move(verdict));
    }
    return view;
  }

  const GroupRosterStatement& s_;
  const int64_t now_;
  std::map<std::string, const GroupRosterStatementMember*> s_members_;
  std::map<std::string, const GroupRemovedMember*> s_removed_;

  std::map<std::string, const CheckedGroupChange*> held_;
  std::map<std::string, GroupChangeVerdict> verdicts_;
  Changes adds_, renames_, link_adds_, link_revokes_;
  std::map<std::string, Changes> removes_of_, leaves_by_, kept_by_remove_, kept_by_leave_;
  /** Accounts some held add (proved or not) names: a leave by one of them may matter. */
  std::set<std::string> joining_;

  std::map<std::string, bool> removes_memo_, leaves_memo_, remove_kept_memo_, leave_kept_memo_, removed_memo_;
  std::set<std::pair<const void*, std::string>> visiting_;

  std::set<std::string> in_, present_, accepted_;
  std::map<std::string, Reason> static_reasons_;
  std::map<std::string, const CheckedGroupChange*> joined_by_;
  std::string title_;
  uint64_t title_version_ = 0;
  std::map<std::string, GroupInviteLink> links_;
  std::set<std::string> revoked_;
};

} // namespace

std::vector<CheckedGroupChange> CheckChanges(const std::vector<GroupChange>& changes, const GroupCardLookup& cards,
                                             const int64_t now_ms) {
  std::map<std::string, std::vector<AccountCard>> by_signer;
  for (const GroupChange& c : changes) {
    if (by_signer.count(c.signer) == 0) {
      by_signer[c.signer] = ProvedCards(c.signer, changes, cards, now_ms);
    }
  }
  std::vector<CheckedGroupChange> checked;
  checked.reserve(changes.size());
  for (const GroupChange& c : changes) {
    checked.push_back(CheckOne(c, by_signer[c.signer]));
  }
  return Collapse(checked);
}

GroupRosterView ComputeRosterChecked(const GroupRosterStatement& latest, const std::vector<CheckedGroupChange>& changes,
                                     const int64_t now_ms) {
  const std::vector<CheckedGroupChange> collapsed = Collapse(changes);
  return RosterRules(latest, now_ms).Run(collapsed);
}

GroupRosterView ComputeRoster(const GroupRosterStatement& latest, const std::vector<GroupChange>& changes,
                              const GroupCardLookup& cards, const int64_t now_ms) {
  return ComputeRosterChecked(latest, CheckChanges(changes, cards, now_ms), now_ms);
}

bool FoldsIn(const GroupChangeVerdict& verdict) {
  switch (verdict.status) {
    case Status::Applied:
    case Status::Superseded:
    case Status::Undone:
      return true;
    case Status::Dropped:
      return false;
    case Status::Void:
      break;
  }
  switch (verdict.reason) {
    case Reason::DeviceOffCard:
    case Reason::SignerOut:
    case Reason::NotAllowed:
    case Reason::RemovedMember:
    case Reason::NotReadmitted:
    case Reason::GroupFull:
    case Reason::DuplicateLink:
      return true;
    default:
      return false;
  }
}

GroupRosterStatement FoldChanges(const GroupRosterStatement& current, const GroupRosterView& view) {
  GroupRosterStatement next = current;
  next.roster_epoch = current.roster_epoch + 1;
  next.owner_signature_b64.clear();
  if (current.dissolved) {
    return next;
  }
  next.members.clear();
  for (const GroupRosterViewMember& m : view.members) {
    next.members.push_back({.identity = m.identity, .role = m.role, .joined_at = m.joined_at, .added_by = m.added_by});
  }
  next.removed = view.removed;
  next.links = view.links;
  next.title = view.title;
  next.title_version = view.title_version;
  std::set<std::string> folded(current.folded.begin(), current.folded.end());
  for (const GroupChangeVerdict& v : view.verdicts) {
    if (FoldsIn(v)) {
      folded.insert(v.id);
    }
  }
  next.folded.assign(folded.begin(), folded.end());
  return next;
}

bool GroupRosterView::Lists(const std::string& identity) const {
  return Find(identity) != nullptr;
}

const GroupRosterViewMember* GroupRosterView::Find(const std::string& identity) const {
  for (const GroupRosterViewMember& m : members) {
    if (m.identity == identity) {
      return &m;
    }
  }
  return nullptr;
}

const GroupChangeVerdict* GroupRosterView::Verdict(const std::string& change_id) const {
  for (const GroupChangeVerdict& v : verdicts) {
    if (v.id == change_id) {
      return &v;
    }
  }
  return nullptr;
}

std::vector<std::string> GroupRosterView::HeldIds() const {
  std::vector<std::string> ids;
  for (const GroupChangeVerdict& v : verdicts) {
    if (v.status != GroupChangeStatus::Dropped) {
      ids.push_back(v.id);
    }
  }
  return ids;
}

} // namespace pbr
