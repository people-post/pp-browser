#pragma once

#include "foundation/crypto/CryptoTypes.h"
#include "lib/delivery/PeerSessionKey.h"

#include "common/Error.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

struct RetiredPskEntry {
  uint32_t epoch = 0;
  std::string master_psk_b64;
  int64_t retired_at = 0;
};

struct PskBundleV1 {
  CryptoChannel channel = CryptoChannel::E2e;
  uint32_t active_epoch = 1;
  std::string master_psk_b64;
  std::vector<RetiredPskEntry> retired_epochs;
};

struct PeerSessionRecord {
  PeerSessionKey key;
  uint32_t session_epoch = 1;
  std::optional<std::string> master_psk_b64;
  std::optional<std::string> psk_fingerprint;
  std::optional<int64_t> psk_verified_at;
  std::vector<RetiredPskEntry> retired_psks;
  PublicKeyScope key_scope = PublicKeyScope::Account;
  std::optional<std::string> thread_kem_pk_b64;
  std::optional<std::string> thread_kem_sk_b64;
  std::optional<std::string> peer_thread_kem_pk_b64;
  std::optional<int64_t> last_psk_rotate_at;
  uint32_t psk_rotate_msg_count = 0;
  std::optional<std::string> last_rotation_id;
  /**
   * e2e_public AutoKey: the session's id (hash of the key_init that opened it), and — while the peer
   * has not yet been heard under a session we opened — that key_init, attached to every send until
   * it has been.
   */
  std::optional<std::string> auto_key_session_id;
  std::optional<std::string> auto_key_init_pending;
  /**
   * The latest signed timestamp (the peer's clock) of an envelope the peer sent under this session.
   * A different session's key_init is adopted only from a newer envelope, so a late or replayed
   * opener cannot roll the pair back to a session one side has left.
   */
  std::optional<int64_t> auto_key_peer_heard_ms;
  /**
   * This device's numbering in the pair was lost (the session was kept through *Reset chats*, which
   * deletes the threads): the next send starts the pair's next epoch at seq 1 rather than rewinding in
   * this one (D014, #378).
   */
  bool restart_pending = false;
};

/**
 * Where a device keeps its peer sessions (CR024): the port is the rows — load, list, save, and the device pair's
 * numbering, handed out atomically. What the rows mean (an epoch's key, verification, a key bundle) is delivery's,
 * on top of them. The app's storage keeps them (encrypted at rest); tests use an in-memory one.
 */
class IPeerSessionStore {
public:
  virtual ~IPeerSessionStore() = default;

  virtual Roe<std::optional<PeerSessionRecord>> Load(const PeerSessionKey& key) const = 0;
  /** Sessions with a master PSK or a retired tail. */
  virtual Roe<std::vector<PeerSessionRecord>> List() const = 0;
  virtual Roe<void> Save(const PeerSessionRecord& record) = 0;
  /**
   * Our next `sender_seq` in a device pair's session at its current epoch (M021: numbering is the pair's;
   * a new epoch starts at 1). Device-pair keys only; the session must exist.
   */
  virtual Roe<uint64_t> AllocateSenderSeq(const PeerSessionKey& key) = 0;

  /** A fresh random master PSK. */
  Roe<ByteVector> GenerateMasterPsk() const;
  /** The master PSK of `envelope_epoch`: the current one, or a retired one; none when neither has it. */
  Roe<std::optional<std::string>> ResolveMasterPskForEpoch(const PeerSessionKey& key, uint32_t envelope_epoch) const;
  Roe<void> MarkPskVerified(const PeerSessionKey& key, int64_t verified_at_ms);
  Roe<bool> IsPskVerified(const PeerSessionKey& key) const;
  Roe<PskBundleV1> ExportPskBundle(const PeerSessionKey& key) const;
  /** Adopts the bundle's key (unverified), merging its retired tail into the session's and keeping its KEM state. */
  Roe<void> ImportPskBundle(const PeerSessionKey& key, const PskBundleV1& bundle);
};

} // namespace pbr
