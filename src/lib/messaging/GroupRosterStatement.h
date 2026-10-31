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
  /** Sorted by identity; exactly one has role owner, the signer. */
  std::vector<GroupRosterStatementMember> members;
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
};

namespace group_roster_statement {

/** The bytes the owner signs: domain tag, group, epoch, owner, members (length-prefixed fields). */
ByteVector SignBytes(const GroupRosterStatement& statement);
/** The bytes a transferring owner signs: domain tag, group, from, to, epoch. */
ByteVector TransferSignBytes(const std::string& group_id, const GroupOwnerTransfer& transfer);

/**
 * Sign `members` at `roster_epoch` as the group's owner (`signer`'s account, `public_key_b64` its key):
 * sorts the members and fills the owner fields and signature; `owner_chain` is carried as given.
 */
Roe<GroupRosterStatement> Sign(IAccountSigningAccess& signer, const std::string& public_key_b64,
                               const std::string& group_id, uint64_t roster_epoch,
                               std::vector<GroupRosterStatementMember> members,
                               std::vector<GroupOwnerTransfer> owner_chain, bool dissolved = false);

/** Transfer the group to `to_identity` at `roster_epoch`, signed by the owner giving it up. */
Roe<GroupOwnerTransfer> SignTransfer(IAccountSigningAccess& signer, const std::string& public_key_b64,
                                     const std::string& group_id, const std::string& to_identity,
                                     uint64_t roster_epoch);

/**
 * Valid for a device that knows `known_owner` (empty: knows none): the one owner member signed it with
 * the key its account id comes from, members are unique, every transfer in the chain is signed by the
 * owner it moves from, links to the next, and its epochs rise to at most the statement's; and, when
 * `known_owner` is not the statement's owner, the chain moves ownership away from `known_owner`.
 */
Roe<void> Verify(const GroupRosterStatement& statement, const std::string& known_owner);

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
