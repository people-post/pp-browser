#pragma once

#include "common/Error.h"
#include "common/Module.h"
#include "lib/calls/ICallMediaKeyStore.h"
#include "lib/calls/ICallSessionReader.h"
#include "foundation/crypto/CryptoTypes.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * M024 bridge, sending: a media key also goes in its old form, wrapped under the account pair's session
 * (`wrapped_key_b64`, V015), beside the key itself (`media_key_b64`), for an app before M024 that reads only that.
 * The lane copy each device reads is sealed in that device's own session, which is the wrap. When it goes and what
 * goes with it: docs/contracts/COMPATIBILITY.md § Bridges (step 1).
 */
inline constexpr bool kMediaKeyAccountWrapBridge = true;
/**
 * M024 bridge, reading: a message with no `media_key_b64` (a sender before M024) has its `wrapped_key_b64` unwrapped
 * under the account pair's session held. Goes a release after kMediaKeyAccountWrapBridge (COMPATIBILITY § Bridges,
 * step 2): a sender on the bridge release may still send only the old form.
 */
inline constexpr bool kMediaKeyAccountWrapRead = true;

/** A call media key's size (ICallMediaKeyStore::GenerateEpochKey). */
inline constexpr size_t kCallMediaKeyBytes = 32;

/**
 * A call's media epoch keys between the peers: mint (a new call, a rotation), put into an invite,
 * send as CallMediaKey (on accept, on resend, after a rotation), and take a peer's key in (invite or
 * CallMediaKey) — stored, then "key ready" so a 1:1 start waiting on it (V015) goes on. The key rides
 * the control-lane message itself (M024: each copy sealed for one device of the peer's); for one release
 * also wrapped under the account pair's session (kMediaKeyAccountWrapBridge). The call session workflow
 * decides when, and records the epoch on the call's row (the rows' one writer, V055); this does the key
 * work. Calls owner.
 */
class CallMediaKeyExchange : public Module {
public:
  /**
   * What it needs from the control lane: the account pair's E2E session key (the old form's wrap, M024) — opened
   * when missing to wrap, only the one held to unwrap — and a CallMediaKey send.
   */
  struct Transport {
    std::function<Roe<ByteVector>(const std::string& peer_identity)> resolve_peer_session_key;
    std::function<Roe<ByteVector>(const std::string& peer_identity)> held_peer_session_key;
    std::function<Roe<void>(const std::string& call_id, const std::string& peer_identity, uint32_t media_epoch,
                            const std::string& media_key_id, const ByteVector& key)>
        send_media_key;
  };
  /** One epoch key of a call. */
  struct EpochKey {
    uint32_t epoch = 0;
    std::string key_id;
    ByteVector key;
  };
  /** A peer's key taken in: for the session workflow to move the call to (StepFor). */
  struct TakenKey {
    std::string call_id;
    uint32_t epoch = 0;
    std::string key_id;
  };
  /** What a key at an epoch does to a call on another: epochs only move forward. */
  enum class EpochStep {
    /** An older epoch (late or replayed): not the key now. */
    Ignore,
    /** The current epoch's key again (a resend): install it again — how a member that missed it catches up. */
    Reinstall,
    /** A newer epoch: the call moves to it. */
    Advance,
  };
  static EpochStep StepFor(uint32_t current_epoch, uint32_t key_epoch);
  using KeyReadyFn = std::function<void(const std::string& call_id)>;

  CallMediaKeyExchange(const ICallSessionReader& sessions, ICallMediaKeyStore& keys, Transport transport);

  /** Runs after a peer's key is stored (the 1:1 start may be waiting for it). */
  void SetOnKeyReady(KeyReadyFn fn) { on_key_ready_ = std::move(fn); }

  /** A new key for `call_id` at `epoch`, stored. The caller records epoch / key id on the session. */
  Roe<EpochKey> Mint(const std::string& call_id, uint32_t epoch);
  /** The next epoch's key (the session's epoch + 1), stored; the caller records it on the session. */
  Roe<EpochKey> Rotate(const std::string& call_id);
  /** The call's current key (per its session). */
  Roe<EpochKey> Current(const std::string& call_id) const;

  /**
   * `key` in its old form for `peer_identity` (wrapped under the account pair's session) — embedded in the invite
   * beside the key itself while kMediaKeyAccountWrapBridge; empty when it cannot be wrapped, or after the bridge.
   */
  std::string WrapForPeer(const std::string& call_id, const EpochKey& key, const std::string& peer_identity);

  /** Send `key` to `peer_identity` as CallMediaKey. */
  Roe<void> Send(const std::string& call_id, const std::string& peer_identity, const EpochKey& key);
  /** Send the call's current key to `peer_identity`. */
  Roe<void> SendCurrent(const std::string& call_id, const std::string& peer_identity);

  /**
   * Take the key `sender_identity` sent us (`what`: where it came, for the log): the key itself when the message
   * carries it (M024), else its old form, unwrapped under the account pair's session (an app before M024).
   * Stored: key ready runs, and true.
   */
  bool TakeKey(const std::string& call_id, uint32_t epoch, const std::string& key_id, const std::string& media_key_b64,
               const std::string& wrapped_key_b64, const std::string& sender_identity, const char* what);

  /** An inbound CallMediaKey: its key, once taken (nothing when it cannot be opened yet). */
  Roe<std::optional<TakenKey>> HandleInbound(const std::string& detail_json, const std::string& sender_identity);

private:
  /** The key a message carries: the key itself, else its old form unwrapped; none (logged) when neither opens. */
  std::optional<ByteVector> OpenKey(const std::string& call_id, uint32_t epoch, const std::string& key_id,
                                    const std::string& media_key_b64, const std::string& wrapped_key_b64,
                                    const std::string& sender_identity, const char* what);

  const ICallSessionReader& sessions_;
  ICallMediaKeyStore& keys_;
  Transport transport_;
  KeyReadyFn on_key_ready_;
};

} // namespace pbr
