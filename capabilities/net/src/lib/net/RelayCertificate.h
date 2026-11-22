#pragma once

#include "foundation/crypto/CryptoTypes.h"
#include "foundation/crypto/RelayId.h"

#include "common/Error.h"
#include "common/ExtArea.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * The relay operator set and the relay certificate (release-compatibility SIGNED_FORMATS § 3;
 * docs/contracts/SERVICE_ENDPOINTS.md § Relay identity), after pp-ledger's beacon authority: an offline **operator
 * set** (keys and a threshold) certifies the relay's hot **transport key** (its PeerId, the audience of every signed
 * call) and **signing key**; the relay's own keys certify nothing, so a stolen one cannot move clients to a thief's.
 * Clients pin the founding set as its hash, the relay id (RelayId.h), and keep per relay id the valid certificate of
 * the highest `sequence` they have seen, from any channel (`relay-identity`, `client-compat`, the build).
 *
 *   OperatorSet  = u8 threshold ‖ u8 n ‖ n × LenBytes(typed signature key)      sorted by bytes, distinct, 1 ≤ t ≤ n ≤ 8
 *   certificate  = u8 version (1) ‖ LenBytes body ‖ u8 m ‖ m × (u8 signer_index ‖ LenBytes typed signature)
 *   signed       = "pp-browser:relay-certificate" 00 ‖ u8 1 ‖ body
 *   body         = LenBytes OperatorSet_founding ‖ u64 k ‖ k × LenBytes(operator-set change)
 *                ‖ u64 sequence ‖ i64 issued_at ‖ i64 expires_at ‖ u64 o ‖ o × LenUtf8 origin
 *                ‖ LenBytes transport key (typed) ‖ LenBytes signing key (typed) ‖ ext area
 *   change       = u8 version (1) ‖ LenBytes body ‖ u8 m ‖ m × (u8 signer_index ‖ LenBytes typed signature)
 *   signed       = "pp-browser:relay-operator-set-change" 00 ‖ u8 1 ‖ relay id (32) ‖ body
 *   body         = u64 set_generation ‖ LenBytes OperatorSet_new ‖ ext area
 *
 * Integers big endian, `LenBytes` / `LenUtf8` a u64 length (the pp binary wire profile), the ext area as every signed
 * object's (SIGNED_EXTENSIONS § The extension area). `certificate_b64` is standard base64 of the blob.
 */

inline constexpr uint8_t kRelayCertificateVersion = 1;
inline constexpr uint8_t kOperatorSetChangeVersion = 1;
inline constexpr char kRelayCertificateDomain[] = "pp-browser:relay-certificate";
inline constexpr char kOperatorSetChangeDomain[] = "pp-browser:relay-operator-set-change";
/** An operator set holds 1 to 8 keys. */
inline constexpr size_t kMaxOperatorKeys = 8;
/** A certificate binds 1 to 4 origins (Q6): where the relay answers. */
inline constexpr size_t kMaxCertificateOrigins = 4;
/** The founding set is generation 1; each change carries the next. */
inline constexpr uint64_t kFoundingSetGeneration = 1;
/** At most this many set changes in one certificate (a reader bound: release 1's operations write none). */
inline constexpr size_t kMaxOperatorSetChanges = 16;
/** A certificate blob larger than this is refused unread (2 of 3 is about 16.6 KB; 8 keys and 8 changes fit). */
inline constexpr size_t kMaxRelayCertificateBytes = 256 * 1024;

/** The operator set: its threshold and its typed ML-DSA keys, sorted by their bytes. */
struct OperatorSet {
  uint8_t threshold = 0;
  std::vector<ByteVector> keys;

  /** `u8 threshold ‖ u8 n ‖ n × LenBytes(key)`, as it is hashed and carried. */
  ByteVector Encode() const;
  /** The relay id of this set as the founding one. */
  RelayId Id() const { return RelayId::OfOperatorSet(Encode()); }
  /** The index of `typed_key` in the set; nullopt when it is not one of its keys. */
  std::optional<size_t> IndexOf(std::span<const uint8_t> typed_key) const;

  bool operator==(const OperatorSet&) const = default;
};

/**
 * A set of `typed_keys` (typed signature keys, any order) at `threshold`: sorted by bytes. Refused: no key or more than
 * 8, a key twice, a threshold outside 1..n, a key that is not a typed signature key this build reads.
 */
Roe<OperatorSet> MakeOperatorSet(uint8_t threshold, std::vector<ByteVector> typed_keys);

/** One operator's signature on a certificate or a set change: its index in the set that signs, and the signature. */
struct OperatorSignature {
  uint8_t signer_index = 0;
  /** Typed (an ML-DSA-65 signature: 3310 bytes). */
  ByteVector signature;

  bool operator==(const OperatorSignature&) const = default;
};

/** An operator-set change: the set in force replaces itself (a lost or leaked operator key); the relay id stays. */
struct OperatorSetChange {
  /** 2 for the first change, then 3, …: one above the set it replaces. */
  uint64_t set_generation = 0;
  OperatorSet new_set;
  pp::ExtArea ext;
  /** Signed by `threshold` distinct operators of the set it replaces. */
  std::vector<OperatorSignature> signatures;

  bool operator==(const OperatorSetChange&) const = default;
};

/** What the operator set certifies (the certificate's body). */
struct RelayCertificateBody {
  /** The founding set: its hash is the relay id. */
  OperatorSet founding;
  /** The set changes in order (none in release 1's operations, § 3.6). */
  std::vector<OperatorSetChange> changes;
  /** ≥ 1, rising: clients keep the highest. */
  uint64_t sequence = 0;
  /** Unix ms; informational. */
  int64_t issued_at = 0;
  /** Unix ms; 0 = no expiry (release 1 writes 0, Q7). */
  int64_t expires_at = 0;
  /** 1 to 4 relay base URLs (`https://host/api/relay`): where this relay answers (Q6). */
  std::vector<std::string> origins;
  /** Typed ML-DSA key: its PeerId is the audience of signed calls. */
  ByteVector transport_key;
  /** Typed ML-DSA key: hot on the deployment; signs nothing in release 1 (Q13). */
  ByteVector signing_key;
  pp::ExtArea ext;

  bool operator==(const RelayCertificateBody&) const = default;
};

/** A relay certificate as read (or made): its fields, its signatures and its bytes as they came. */
struct RelayCertificate {
  RelayCertificateBody body;
  std::vector<OperatorSignature> signatures;
  /** The blob, exactly as received or written: what is stored and forwarded (unknown entries kept). */
  ByteVector blob;
  /** The founding set's hash. */
  RelayId relay_id;
  /** The set in force: the founding set after the changes (the certificate's signer indices point into it). */
  OperatorSet set_in_force;
  /** The transport key's PeerId (pp::PeerIdFromMlDsaPublicKey): the audience every signed call names. */
  std::string audience_peer_id;

  /** Standard base64 of the blob (`certificate_b64`, `relay_certificate_b64`). */
  std::string ToBase64() const;
  /** The certificate lists `origin` (as relays compare, RelayOriginKey). */
  bool Lists(const std::string& origin) const;
  bool operator==(const RelayCertificate& other) const { return blob == other.blob; }
};

/** How a certificate read, in the receiver's order (SIGNED_EXTENSIONS § The receiver's order) and § 3.2's rules. */
enum class RelayCertificateVerdict : uint8_t {
  Accepted = 0,
  /** Version missing or 0: made by an older tool. Refused. */
  Older = 1,
  /** A newer version (of it or of a set change), an unknown algorithm id or an unknown critical entry. */
  NeedsUpdate = 2,
  /** Fields that do not parse: sizes, bounds, a set out of order, a signer index outside the set or twice. */
  Malformed = 3,
  /** A signature that does not verify, too few signatures, a set change out of order or not signed by its set. */
  Forged = 4,
  /** Its founding set is another relay's (not the pinned relay id): it does not match the relay's address. */
  OtherRelay = 5,
  /** `expires_at` is past. */
  Expired = 6,
  /** Verifies but breaks § 3.2: sequence 0, an origin out of bounds or twice, keys not distinct or an operator's. */
  Invalid = 7,
};

/** "accepted", "older", "needs_update", "malformed", "forged", "other_relay", "expired", "invalid". */
std::string_view RelayCertificateVerdictName(RelayCertificateVerdict verdict);

/** What reading a certificate found. */
struct RelayCertificateCheck {
  RelayCertificateVerdict verdict = RelayCertificateVerdict::Malformed;
  /** Accepted: the certificate. */
  std::optional<RelayCertificate> certificate;
  /** Why it was refused, for the log (empty when accepted). */
  std::string detail;

  bool accepted() const { return verdict == RelayCertificateVerdict::Accepted; }
  /**
   * NeedsUpdate → AppError::NewerFormat; any other refusal → Err::Network::RelayIdentityUnavailable naming the
   * verdict (the relay does not match its address: nothing is signed for it). Only meaningful when not accepted.
   */
  Error ToError() const;
};

/**
 * Reads a certificate blob and checks it (SIGNED_FORMATS § 3.2): version 1; its founding set hashes to `pinned`
 * (empty: any relay id — a first use, whose relay id is then the certificate's own); each set change, in order, one
 * generation above the last and signed by `threshold` distinct operators of the set it replaces; `threshold` distinct
 * valid signatures of the set in force; no unknown critical entry (the certificate's or a change's); then sequence ≥ 1,
 * 1 to 4 distinct origins within the card's bounds (http too: the local stack), transport and signing keys distinct
 * and neither an operator's, `expires_at` 0 or after `now_ms`. Every signature carried must verify.
 */
RelayCertificateCheck ReceiveRelayCertificate(std::span<const uint8_t> blob, const RelayId& pinned, int64_t now_ms);
/** The same from standard base64 (not base64: Malformed). */
RelayCertificateCheck ReceiveRelayCertificateB64(const std::string& certificate_b64, const RelayId& pinned,
                                                 int64_t now_ms);

/** A certificate's origin is within the card's origin bounds (IsCardRelayOrigin); `http://` too, measured as https. */
bool IsCertificateOrigin(const std::string& origin);

/** What a valid certificate offered does to the one held for its relay id (the highest is kept, never a lower). */
enum class RelayCertificateOffer : uint8_t {
  /** None held, or a higher sequence: kept now. */
  Adopted = 0,
  /** The certificate held, byte for byte. */
  Same = 1,
  /** Below the sequence held: refused (never accepted). */
  Lower = 2,
  /** The sequence held with other bytes: refused and logged as an error (the first stays; operators never sign two). */
  Conflict = 3,
};

/** "adopted", "same", "lower", "conflict". */
std::string_view RelayCertificateOfferName(RelayCertificateOffer offer);

/** What keeping `offered` (valid, of the same relay id) does when `held` is held (null: none). */
RelayCertificateOffer CompareRelayCertificates(const RelayCertificate* held, const RelayCertificate& offered);

// Making them: the operators' tool (pp-relay-apis `scripts/relay-operator.mjs`), the vectors and tests.

/** The signed bytes of a certificate body: `"pp-browser:relay-certificate" 00 ‖ u8 1 ‖ body`. */
ByteVector RelayCertificateSignedBytes(const RelayCertificateBody& body);
/** The signed bytes of a set change of relay `relay_id`: `"…-operator-set-change" 00 ‖ u8 1 ‖ relay id ‖ body`. */
ByteVector OperatorSetChangeSignedBytes(const RelayId& relay_id, const OperatorSetChange& change);
/** The body's encoding (what the certificate's LenBytes body holds). */
ByteVector EncodeRelayCertificateBody(const RelayCertificateBody& body);
/** A certificate blob: `version ‖ LenBytes body ‖ u8 m ‖ signatures`, with `version` as given (tests write others). */
ByteVector EncodeRelayCertificate(const RelayCertificateBody& body, const std::vector<OperatorSignature>& signatures,
                                  uint8_t version = kRelayCertificateVersion);
/** A set change blob: `version ‖ LenBytes body ‖ u8 m ‖ signatures`. */
ByteVector EncodeOperatorSetChange(const OperatorSetChange& change, uint8_t version = kOperatorSetChangeVersion);

/** One operator who signs: its index in the signing set and its raw ML-DSA-65 secret key. */
struct OperatorSigner {
  uint8_t signer_index = 0;
  ByteVector secret_key;
};

/** `signers` sign the set change of relay `relay_id` (its fields as given; signatures replaced). */
Roe<OperatorSetChange> SignOperatorSetChange(const RelayId& relay_id, OperatorSetChange change,
                                             const std::vector<OperatorSigner>& signers);
/** `signers` (of the set in force) sign `body`: the certificate's blob, as EncodeRelayCertificate writes it. */
Roe<ByteVector> SignRelayCertificate(const RelayCertificateBody& body, const std::vector<OperatorSigner>& signers);

} // namespace pbr
