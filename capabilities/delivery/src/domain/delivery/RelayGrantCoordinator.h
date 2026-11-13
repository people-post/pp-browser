#pragma once

#include "common/Module.h"
#include "common/chat/RelayGrant.h"
#include "common/chat/SystemControl.h"
#include "lib/delivery/HttpRelayClient.h"
#include "lib/delivery/IRelayGrantStore.h"
#include "lib/delivery/RelayGrantNotice.h"

#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/PbrCompat.h"

namespace pbr {

class AccountKeyResolver;
class ControlLane;
class IdentityStore;

/**
 * This account's relay grants (multi-relay MR005). A relay cannot see contact lists: it takes a send as a contact's
 * when it carries the recipient's grant, else as a stranger's (the recipient's price, or a small quota and no push).
 *
 * - **Issue:** each granted peer (a contact not blocked, a member of a group we are in — the hub says who) gets a
 *   grant, 30 days long, renewed once less than kRelayGrantRenewBeforeMs is left; none for a peer no longer granted
 *   (Stop: blocked, removed). It goes over the control lane, peer to peer; the grantee confirms it (`relay_grant_ack`).
 *   One not confirmed goes again when its grantee is heard from (it is online then), at most once an hour; one
 *   asked for (`relay_grant_ask`) goes at once.
 * - **Hold:** grants given to us, checked with the grantor's account key, attached to each relay send to its inbox
 *   (SendHooks); one the relay refuses is dropped and asked for again, the send going once as a stranger's meanwhile.
 *   A granted peer we have held no usable grant from for a few minutes is asked for one when heard from, at most
 *   every six hours (not on a send: an ask is a stranger's send too, and the relay allows strangers few). When a grant
 *   arrives, sends to its grantor held back by the stranger quota go at once.
 * - Self-sends need none.
 *
 * Any thread.
 */
class RelayGrantCoordinator : public Module {
public:
  RelayGrantCoordinator(IdentityStore& identity, AccountKeyResolver& peer_keys, ControlLane& lane);

  /** Where the grants are kept (profile.db); none: nothing is issued or attached. */
  void SetStore(IRelayGrantStore* store);
  /** Who we grant: a contact not blocked, a member of a group we are in. None: nobody. */
  void SetGrantedPeer(std::function<bool(const std::string& account_id)> granted);
  /** Called with the grantor once a grant from it is kept (the outbox sends what the stranger quota held back). */
  void SetOnGrantReceived(std::function<void(const std::string& grantor)> on_received);
  void SetClockForTest(std::function<int64_t()> now) { now_ = std::move(now); }

  /** For the relay client: the grant to attach, the refusal to meet, our key (HttpRelayClient). */
  RelaySendHooks SendHooks();
  /** The lane family taking `relay_grant` / `relay_grant_ack` / `relay_grant_ask` off the control lane. */
  SystemControlFamily Family();

  /** Grant each of `accounts` that is a granted peer and holds no current grant of ours: issue, or renew. Blocking. */
  void EnsureGranted(const std::vector<std::string>& accounts);
  /** Stop renewing the grant to `account` (blocked, removed from our groups); the one it holds runs out. */
  void Stop(const std::string& account);
  /** An envelope from `account` verified: it is online, so an unconfirmed grant goes again and a missing one is asked for. */
  void NoteHeardFrom(const std::string& account);

  /** The usable grant `recipient` gave us, if any. SendHooks' `grant_for`. */
  std::optional<RelayGrant> GrantFor(const std::string& recipient);
  /** The relay refused the grant `recipient` gave us. SendHooks' `grant_refused`. */
  void GrantRefused(const std::string& recipient);
  /** A notice from `sender` (the lane's verified sender). Family's `apply`. */
  Roe<void> OnNotice(const relay_grant::Notice& notice, const std::string& sender);

private:
  int64_t Now() const;
  bool Granted(const std::string& account) const;
  std::string OwnAccount() const;
  /** A new grant to `account`, kept as issued (unconfirmed); none when it could not be signed or kept. */
  std::optional<RelayGrant> IssueTo(const std::string& own, const std::string& account);
  /** Send `grants` over the control lane, each to its grantee. */
  void SendGrants(std::vector<RelayGrant> grants);
  /** Ask `account` for a grant, unless asked lately (`refused`: the relay just refused ours — a shorter wait). */
  void Ask(const std::string& account, bool refused);
  /** True (and noted) when `key` was not acted on in the last `every_ms` in `when`. */
  bool Due(std::unordered_map<std::string, int64_t>& when, const std::string& key, int64_t every_ms);

  IdentityStore& identity_;
  AccountKeyResolver& peer_keys_;
  ControlLane& lane_;
  IRelayGrantStore* store_ = nullptr;
  std::function<bool(const std::string&)> granted_;
  std::function<int64_t()> now_;
  mutable std::mutex mutex_;
  /** Per account: when we last asked it for a grant, sent it ours again, looked at it on hearing from it. */
  std::unordered_map<std::string, int64_t> asked_at_;
  std::unordered_map<std::string, int64_t> resent_at_;
  std::unordered_map<std::string, int64_t> heard_at_;
  /** Per granted peer: since when we have held no usable grant from it (noted when heard from). */
  std::unordered_map<std::string, int64_t> lacking_since_;
  std::function<void(const std::string&)> on_grant_received_;
};

} // namespace pbr
