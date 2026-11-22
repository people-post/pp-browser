#pragma once

#include "common/thread/ThreadTypes.h"
#include "foundation/crypto/SignedReceive.h"
#include "lib/delivery/TypedSigning.h"
#include "common/Error.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * Envelope v4's signed bytes, signature and receipt (release-compatibility SIGNED_FORMATS § 2.1; docs/contracts/
 * MESSAGE_ENCRYPTION.md § Canonical relay-envelope signing bytes), signed by the sending device's ML-DSA-65 key:
 *
 *   "pp-browser:relay-envelope" 00 ‖ u8 4
 *   ‖ u8 route_kind ‖ u8 channel ‖ i64 timestamp
 *   ‖ LenUtf8 message_id ‖ LenUtf8 sender_contact_id ‖ LenUtf8 sender_device
 *   ‖ u64 copies, by recipient_device: LenUtf8 recipient_device ‖ u32 session_epoch ‖ u64 sender_seq
 *                                      ‖ LenUtf8 key_init_b64 ("" when none) ‖ copy hash (32)
 *   ‖ ext area
 */
class EnvelopeSigner {
public:
  /** The domain: no `-vN` (the version is the byte after it). */
  static constexpr char kDomain[] = "pp-browser:relay-envelope";

  /** The signed bytes of `envelope` at its `envelope_version` (only 4 is built: any other is refused). */
  static Roe<std::vector<uint8_t>> BuildSignBytes(const RelayEnvelope& envelope);
  /** BLAKE2b-256 over exactly what a device decrypts in one copy: 0x02 ‖ LenBytes(payload). */
  static Roe<std::vector<uint8_t>> CopyHash(const RelayCopy& copy);
  /**
   * 1 to kMaxRelayCopies copies, one per device (at most one account copy) — on the control lane too: a copy per
   * device of the peer's, or the account copy (M024). Anything else is malformed — signing and verifying both refuse
   * it. (No route names a group: a group copy carries its group id inside its ciphertext, G018 item 7.)
   */
  static Roe<void> CheckBodyShape(const RelayEnvelope& envelope);
  /**
   * Build the signed bytes and sign them with `sign` (the sending device's raw signer, `IdentityStore::SignDeviceBytes`):
   * `envelope.signature` is the typed signature, base64.
   */
  static Roe<void> Sign(RelayEnvelope& envelope, const RawSignFn& sign);
  /**
   * Step 3 of the receiver's order for every typed blob the envelope carries: the signature, and each copy's
   * `key_init_b64` (KEM) and `payload_b64` (AEAD). The first algorithm id this build does not know: why the envelope
   * is a newer app's (it is read as a higher version is, nothing verified); none: nullopt; a malformed blob: an error.
   */
  static Roe<std::optional<std::string>> UnknownAlgorithm(const RelayEnvelope& envelope);
  /**
   * The receiver's order (SIGNED_FORMATS § 1.1) for an envelope ParseRelayEnvelope read: its version, its body's shape,
   * the typed signature's and the key's algorithm ids, the signature over the rebuilt bytes by `device_key_b64` (the
   * sending device's key, as its account card holds it), then the extension area's critical entries (none understood
   * at release 1: an unknown one, once the signature verified, is NeedsUpdate). Forged is a signature that does not
   * verify.
   */
  static SignedReceipt Receive(const RelayEnvelope& envelope, const std::string& device_key_b64);

private:
  static uint8_t RouteKindByte(const RelayRoute& route);
  static uint8_t ChannelByte(const RelayRoute& route);
};

} // namespace pbr
