#pragma once

#include "lib/people/AccountCard.h"
#include "foundation/crypto/CryptoTypes.h"
#include "common/Error.h"
#include "common/Value.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/** What a change does to the roster (G018 item 2). */
enum class GroupChangeKind : uint8_t { Add, Leave, Remove, Rename, LinkAdd, LinkRevoke };

/**
 * A roster change a member or admin signs on top of the owner's statement (G018): signed by the signing **device**
 * (item 9), checked against the signer's account card. Which body fields a kind carries:
 *
 * | kind          | body                                                                       |
 * |---------------|----------------------------------------------------------------------------|
 * | `add`         | `member`, `invite_nonce` or `link_id`; a re-admit also `readmits`          |
 * | `leave`       | `adds` (the signer's adds it saw), `keeps` (the signer's unfolded changes) |
 * | `remove`      | `member`, `adds` (that member's adds it saw), `keeps` (the member's changes) |
 * | `rename`      | `title`, `title_version`                                                   |
 * | `link_add`    | `link_id`, `secret_hash`, `expires_at`, `max_uses` (0: no limit)           |
 * | `link_revoke` | `link_id`                                                                  |
 *
 * Fields a kind does not carry are left empty; a change carrying one does not pass CheckShape.
 */
struct GroupChange {
  std::string group_id;
  /** The statement epoch the signer held: display and diagnosis only (validity is R3, against the latest). */
  uint64_t base_epoch = 0;
  /** The signer's account id. */
  std::string signer;
  /** The PeerId of the device that signed it; must be on the signer's card. */
  std::string signer_device;
  /** When it was signed (ms), by the signer's clock: display only. */
  int64_t at = 0;
  GroupChangeKind kind = GroupChangeKind::Add;

  std::string member;
  std::string invite_nonce;
  std::string link_id;
  /** A re-admit: the id of the removal it lifts (G018 R2). */
  std::string readmits;
  /** Change ids; sorted in the signed bytes. */
  std::vector<std::string> adds;
  std::vector<std::string> keeps;
  std::string title;
  uint64_t title_version = 0;
  std::string secret_hash;
  int64_t expires_at = 0;
  uint64_t max_uses = 0;

  /** The signer's account card, carried when the receiver may lack it (not signed: it proves itself). */
  std::optional<AccountCard> signer_card;
  /** ML-DSA-65 by the signing device's key over SignBytes, base64. */
  std::string signature_b64;
};

/** Signs bytes with this device's key (base64 out), as a device signs its envelopes. */
using GroupDeviceSign = std::function<Roe<std::string>(const ByteVector&)>;

namespace group_change {

/** At most this many ids in `adds` or `keeps`. */
inline constexpr size_t kMaxNamedChanges = 256;
/** A rename's title, in bytes. */
inline constexpr size_t kMaxTitleBytes = 256;

std::string KindToString(GroupChangeKind kind);
std::optional<GroupChangeKind> KindFromString(const std::string& value);

/**
 * The bytes the device signs: tag `pp-browser:group-roster-change-v1`, group, base epoch (u64), signer, signer
 * device, at (u64), kind, then the kind's body (DESIGN § Change). Strings are a u32 BE length then the bytes;
 * integers u64 BE; id lists a u64 BE count then each id, sorted.
 */
ByteVector SignBytes(const GroupChange& change);

/** The change id: lowercase hex of BLAKE2b-256(SignBytes). Two copies of one change share it. */
std::string Id(const GroupChange& change);

/** Well formed for its kind: required fields set, foreign fields empty, no repeated ids, bounded lists. */
Roe<void> CheckShape(const GroupChange& change);

/** Fill `signature_b64` with `sign` over SignBytes (the signer and device already set); checks the shape first. */
Roe<GroupChange> Sign(GroupChange change, const GroupDeviceSign& sign);

/**
 * The change verifies against `card`: the card proves itself (VerifyAccountCard at `now_ms`) for the signer's
 * account (its account id derives from the card's account key), lists the signing device, and that device's
 * signing key verifies the signature.
 */
Roe<void> Verify(const GroupChange& change, const AccountCard& card, int64_t now_ms);
/** Only the signature, against `device` (a card entry already proved for the signer). */
Roe<void> VerifyDeviceSignature(const GroupChange& change, const AccountCardDevice& device);

Object ToJson(const GroupChange& change);
Roe<GroupChange> FromJson(const Object& json);

/**
 * G018 item 5: lowercase hex of BLAKE2b-256 over the tag `pp-browser:group-changes-v1`, the count (u64 BE) and the
 * sorted, distinct ids (each u32 BE length then bytes) of the unfolded changes a device holds. Order-free.
 */
std::string ChangesHash(std::vector<std::string> ids);

} // namespace group_change

} // namespace pbr
