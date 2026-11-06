#pragma once

#include "lib/messaging/GroupTypes.h"
#include "foundation/crypto/CryptoTypes.h"
#include "common/Error.h"
#include "common/Value.h"
#include "common/directory/IAccountSigningAccess.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/** One member as the owner states it (G012). */
struct GroupRosterStatementMember {
  std::string identity;
  MemberRole role = MemberRole::Member;
  int64_t joined_at = 0;
  /** G019: who brought them in (the signer of the `add` the owner folded); empty for the group's first members. */
  std::string added_by;
};

/** G018 R2 / G020: an account removed and not re-admitted; a re-admit names `removal`. */
struct GroupRemovedMember {
  std::string identity;
  /** The id of the `remove` change that took them out (GroupChange.h). */
  std::string removal;
  std::string removed_by;
};

/** G021: a live invite link, as its `link_add` put it in the roster. */
struct GroupInviteLink {
  std::string link_id;
  /** Hash of the link's secret (the secret itself never enters the roster). */
  std::string secret_hash;
  /** The account that made it; the link dies when they are no longer in the group. */
  std::string maker;
  int64_t expires_at = 0;
  /** 0: no limit. */
  uint64_t max_uses = 0;
};

/**
 * G018 item 4: a change the owner folded, with the statement epoch it was made against (`base_epoch`, at most the
 * epoch of the statement that folded it): the change floor trims `folded` by it.
 */
struct GroupFoldedChange {
  std::string id;
  uint64_t base_epoch = 0;

  bool operator==(const GroupFoldedChange&) const = default;
};

/**
 * A transfer of ownership, signed by the owner who gave it up (G012 amendment: the owner chain). It
 * lets a device that knows only an earlier owner trust a statement the later owner signed.
 */
struct GroupOwnerTransfer {
  std::string from_identity;
  /** `from_identity`'s account ML-DSA-65 public key (base64); its account id is derived from it. */
  std::string from_public_key_b64;
  std::string to_identity;
  /** The roster epoch the transfer took effect at. */
  uint64_t roster_epoch = 0;
  std::string signature_b64;
};

/**
 * The owner-signed roster of a group (G012): every member stores the latest one and any of them can
 * hand it to a member who lost the group, who checks it offline — the owner's key hashes to the
 * owner's account id, and the chain leads from the owner it knew to this one.
 */
struct GroupRosterStatement {
  std::string group_id;
  uint64_t roster_epoch = 0;
  /** Sorted by identity; exactly one has role owner, the signer; any number admin (G017). */
  std::vector<GroupRosterStatementMember> members;
  /** G017: who brings people in and renames, as the owner set it. */
  GroupPreset preset = GroupPreset::Open;
  /** The group's name and its version (G018 R4: a rename wins on the higher version). */
  std::string title;
  uint64_t title_version = 0;
  /** G018 R2: accounts removed and not re-admitted, sorted by identity. */
  std::vector<GroupRemovedMember> removed;
  /** G021: live invite links, sorted by link id. */
  std::vector<GroupInviteLink> links;
  /**
   * G018 item 4: every change the owner has folded into its statements (this one and earlier ones) whose base epoch
   * is not below `change_floor_epoch`, sorted by id. Devices drop a change it names; carried forward so a device that
   * missed a statement still drops it.
   */
  std::vector<GroupFoldedChange> folded;
  /**
   * G018: a change made against a statement older than this (`base_epoch` below it) is dropped as too old, so
   * `folded` need not name it. The owner raises it as it folds (GroupRosterRules.h, kChangeFloorLag).
   */
  uint64_t change_floor_epoch = 0;
  std::string owner_identity;
  std::string owner_public_key_b64;
  /** Every transfer since the group began, oldest first; empty while its creator owns it. */
  std::vector<GroupOwnerTransfer> owner_chain;
  std::string owner_signature_b64;
  /**
   * G016: the owner's statement that it ended the group, carried by `group_dissolved`. Signed under its own
   * tag, so a roster statement cannot be turned into one (nor the reverse), and never kept as a roster.
   */
  bool dissolved = false;

  bool Lists(const std::string& identity) const;
  /** `folded` names the change `id`. */
  bool Folded(const std::string& id) const;
};

namespace group_roster_statement {

/**
 * The bytes the owner signs (v3, G018): domain tag, group, epoch, owner, members (with `added_by`), preset, title
 * and its version, removed, links, folded (each id with its base epoch), the change floor (length-prefixed fields;
 * every list sorted).
 */
ByteVector SignBytes(const GroupRosterStatement& statement);
/** The bytes a transferring owner signs: domain tag, group, from, to, epoch. */
ByteVector TransferSignBytes(const std::string& group_id, const GroupOwnerTransfer& transfer);

/**
 * Sign `members` at `roster_epoch` as the group's owner (`signer`'s account, `public_key_b64` its key), under
 * `preset`: sorts the members and fills the owner fields and signature; `owner_chain` is carried as given.
 */
Roe<GroupRosterStatement> Sign(IAccountSigningAccess& signer, const std::string& public_key_b64,
                               const std::string& group_id, uint64_t roster_epoch,
                               std::vector<GroupRosterStatementMember> members,
                               std::vector<GroupOwnerTransfer> owner_chain, bool dissolved = false,
                               GroupPreset preset = GroupPreset::Open);

/**
 * Sign `draft` as the group's owner: everything but the owner fields and signature comes from `draft` (e.g. the next
 * statement FoldChanges builds, GroupRosterRules.h); sorts its lists, fills the owner's identity and key, signs.
 */
Roe<GroupRosterStatement> SignStatement(IAccountSigningAccess& signer, const std::string& public_key_b64,
                                        GroupRosterStatement draft);

/** Transfer the group to `to_identity` at `roster_epoch`, signed by the owner giving it up. */
Roe<GroupOwnerTransfer> SignTransfer(IAccountSigningAccess& signer, const std::string& public_key_b64,
                                     const std::string& group_id, const std::string& to_identity,
                                     uint64_t roster_epoch);

/**
 * Valid for a device that knows `known_owner` (empty: knows none): the one owner member signed it with
 * the key its account id comes from, members are unique, the removed are unique and not members, link ids and
 * folded ids are unique, no folded change is below the change floor (nor the floor above the epoch), every transfer
 * in the chain is signed by the
 * owner it moves from, links to the next, and its epochs rise to at most the statement's; and, when
 * `known_owner` is not the statement's owner, the chain moves ownership away from `known_owner`.
 */
Roe<void> Verify(const GroupRosterStatement& statement, const std::string& known_owner);

/**
 * Whether `candidate`'s owner had already handed the group on by the candidate's epoch, as far as `known` — the
 * transfers any statement we hold or are weighing shows — tells: a transfer from the owner at or before the
 * candidate's epoch, with no transfer back to it after that and by that epoch. So a former owner loses to its
 * successor, and an owner who got the group back (A to B to A) wins again.
 */
bool OwnerHandedOn(const std::vector<GroupOwnerTransfer>& known, const GroupRosterStatement& candidate);

/**
 * The statement to adopt among valid ones (G013): drop any signed by an owner a chain shows had
 * already handed the group on at that epoch (a former owner cannot outbid its successor), then the
 * highest roster epoch. Nullopt when none is left.
 */
std::optional<GroupRosterStatement> Pick(const std::vector<GroupRosterStatement>& valid);

Object ToJson(const GroupRosterStatement& statement);
Roe<GroupRosterStatement> FromJson(const Object& json);
Object TransferToJson(const GroupOwnerTransfer& transfer);
Roe<GroupOwnerTransfer> TransferFromJson(const Object& json);

} // namespace group_roster_statement

} // namespace pbr
