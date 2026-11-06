#pragma once

// People who sign a group's roster (G018): each an account (its key signs the owner's statement and its own card)
// with named devices (their keys sign changes), and a book of the cards a device holds.

#include "lib/messaging/GroupChange.h"
#include "lib/messaging/GroupRosterRules.h"
#include "lib/messaging/GroupRosterStatement.h"
#include "lib/people/tests/TestAccount.h"
#include "common/directory/IAccountSigningAccess.h"

#include <gtest/gtest.h>

#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr::test {

inline constexpr int64_t kNowMs = 1'800'000'000'000;
inline constexpr char kGroupId[] = "group:g1";

/** An account in a group: signs statements and cards with its account key, changes with its devices' keys. */
class Person final : public IAccountSigningAccess {
public:
  explicit Person(std::string name) : name_(std::move(name)), account_(TestAccount::Make()) {}

  Roe<std::string> GetAccountId() const override { return account_.account_id; }
  Roe<std::string> SignAccountBytes(const AccountSignBytes& bytes) const override { return account_.Sign(bytes); }

  const std::string& Id() const { return account_.account_id; }
  std::string Key() const { return account_.SigningPublicB64(); }
  const std::string& Name() const { return name_; }

  /** Device `n` of this account (the same keys for the same name in one test binary). */
  const TestDevice& Device(const std::string_view n = "1") const { return NamedDevice(name_ + ":" + std::string(n)); }

  /** The account's card listing `devices`, issued at `issued_at`. */
  AccountCard Card(const std::vector<std::string>& devices = {"1"}, const int64_t issued_at = 1000) const {
    AccountCard card{.keys = account_.Card(), .nickname = name_, .issued_at = issued_at};
    for (const std::string& n : devices) {
      card.devices.push_back(Device(n).Entry());
    }
    return SignAccountCard(card, [this](const ByteVector& bytes) { return account_.Sign(bytes); }).value();
  }

  /** A change of `kind` by this account's device `device`, unsigned. */
  GroupChange Draft(const GroupChangeKind kind, const std::string& device = "1") const {
    GroupChange c;
    c.group_id = kGroupId;
    c.base_epoch = 5;
    c.signer = Id();
    c.signer_device = Device(device).peer_id;
    c.at = kNowMs - 1000;
    c.kind = kind;
    return c;
  }

  GroupChange Signed(GroupChange draft, const std::string& device = "1") const {
    const TestDevice& d = Device(device);
    draft.signer_device = d.peer_id;
    return group_change::Sign(std::move(draft), [&d](const ByteVector& bytes) { return d.Sign(bytes); }).value();
  }

  GroupChange Add(const std::string& member, const std::string& nonce = "invite-1", const std::string& readmits = "",
                  const std::string& device = "1") const {
    GroupChange c = Draft(GroupChangeKind::Add, device);
    c.member = member;
    c.invite_nonce = nonce;
    c.readmits = readmits;
    return Signed(std::move(c), device);
  }

  /** G021: this account's add of `member`, who joined by the link `link_id`. */
  GroupChange AddByLink(const std::string& member, const std::string& link_id, const std::string& device = "1") const {
    GroupChange c = Draft(GroupChangeKind::Add, device);
    c.member = member;
    c.link_id = link_id;
    return Signed(std::move(c), device);
  }

  GroupChange Leave(std::vector<std::string> adds = {}, std::vector<std::string> keeps = {},
                    const std::string& device = "1") const {
    GroupChange c = Draft(GroupChangeKind::Leave, device);
    c.adds = std::move(adds);
    c.keeps = std::move(keeps);
    return Signed(std::move(c), device);
  }

  GroupChange Remove(const std::string& member, std::vector<std::string> adds = {}, std::vector<std::string> keeps = {},
                     const std::string& device = "1") const {
    GroupChange c = Draft(GroupChangeKind::Remove, device);
    c.member = member;
    c.adds = std::move(adds);
    c.keeps = std::move(keeps);
    return Signed(std::move(c), device);
  }

  GroupChange Rename(const std::string& title, const uint64_t version, const std::string& device = "1") const {
    GroupChange c = Draft(GroupChangeKind::Rename, device);
    c.title = title;
    c.title_version = version;
    return Signed(std::move(c), device);
  }

  GroupChange LinkAdd(const std::string& link_id, const int64_t expires_at = kNowMs + 86'400'000,
                      const std::string& device = "1") const {
    GroupChange c = Draft(GroupChangeKind::LinkAdd, device);
    c.link_id = link_id;
    c.secret_hash = "hash-of-" + link_id;
    c.expires_at = expires_at;
    return Signed(std::move(c), device);
  }

  GroupChange LinkRevoke(const std::string& link_id, const std::string& device = "1") const {
    GroupChange c = Draft(GroupChangeKind::LinkRevoke, device);
    c.link_id = link_id;
    return Signed(std::move(c), device);
  }

private:
  std::string name_;
  TestAccount account_;
};

/** The same person for the same name within one test binary (keys are slow to make). */
inline Person& Named(const std::string_view name) {
  static std::map<std::string, std::unique_ptr<Person>> people;
  auto found = people.find(std::string(name));
  if (found == people.end()) {
    found = people.emplace(std::string(name), std::make_unique<Person>(std::string(name))).first;
  }
  return *found->second;
}

/** The newest card a device holds per account. */
struct CardBook {
  std::map<std::string, AccountCard> cards;

  void Hold(const AccountCard& card) { cards[card.keys.account_id] = card; }
  GroupCardLookup Lookup() const {
    return [this](const std::string& account) -> std::optional<AccountCard> {
      auto found = cards.find(account);
      if (found == cards.end()) {
        return std::nullopt;
      }
      return found->second;
    };
  }
};

/** A statement member row. */
inline GroupRosterStatementMember Row(const std::string& identity, const MemberRole role = MemberRole::Member,
                                      const std::string& added_by = "") {
  return {.identity = identity, .role = role, .joined_at = 1000, .added_by = added_by};
}

/** `draft` signed by `owner` as the group's statement (its group and owner chain as given). */
inline GroupRosterStatement SignedStatement(Person& owner, GroupRosterStatement draft) {
  if (draft.group_id.empty()) {
    draft.group_id = kGroupId;
  }
  return group_roster_statement::SignStatement(owner, owner.Key(), std::move(draft)).value();
}

/** Everything a view says, in one string: equal strings are equal rosters. */
inline std::string Describe(const GroupRosterView& view, const bool with_verdicts = true) {
  std::ostringstream out;
  out << "epoch " << view.statement_epoch << " title " << view.title << "@" << view.title_version << "\n";
  for (const GroupRosterViewMember& m : view.members) {
    out << "member " << m.identity << " " << MemberRoleToString(m.role) << " by " << m.added_by << " in " << m.added_in
        << " at " << m.joined_at << "\n";
  }
  for (const GroupRemovedMember& r : view.removed) {
    out << "removed " << r.identity << " " << r.removal << " by " << r.removed_by << "\n";
  }
  for (const GroupInviteLink& l : view.links) {
    out << "link " << l.link_id << " " << l.maker << " " << l.expires_at << "\n";
  }
  if (with_verdicts) {
    for (const GroupChangeVerdict& v : view.verdicts) {
      out << "change " << v.id << " " << static_cast<int>(v.status) << " " << static_cast<int>(v.reason) << "\n";
    }
  }
  return out.str();
}

} // namespace pbr::test
