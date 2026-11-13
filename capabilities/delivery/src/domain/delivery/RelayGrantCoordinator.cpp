#include "domain/delivery/RelayGrantCoordinator.h"

#include "common/Utilities.h"
#include "common/runtime/AppRuntime.h"
#include "domain/delivery/ControlLane.h"
#include "domain/peers/AccountKeyResolver.h"
#include "lib/delivery/RelayGrantSigner.h"
#include "lib/people/ContactIdentity.h"
#include "lib/people/IdentityStore.h"

namespace pbr {

namespace {

/** A granted peer we hold no usable grant from is asked at most this often; after a refusal, sooner. */
constexpr int64_t kAskEveryMs = 6LL * 60 * 60 * 1000;
constexpr int64_t kAskAfterRefusalMs = 10LL * 60 * 1000;
/** An unconfirmed grant goes again at most this often (when its grantee is heard from, or asks). */
constexpr int64_t kResendEveryMs = 60LL * 60 * 1000;
constexpr int64_t kResendOnAskMs = 10LL * 60 * 1000;
/** Hearing from a peer is looked at at most this often (every inbound envelope reports it). */
constexpr int64_t kHeardEveryMs = 10LL * 60 * 1000;
/**
 * A granted peer is asked for its grant only once we have lacked one this long: a peer that just added us sends its
 * grant unasked, and an ask before it lands is a stranger's send the relay counts against a small quota.
 */
constexpr int64_t kAskAfterLackingMs = 2LL * 60 * 1000;

} // namespace

RelayGrantCoordinator::RelayGrantCoordinator(IdentityStore& identity, AccountKeyResolver& peer_keys, ControlLane& lane)
    : identity_(identity), peer_keys_(peer_keys), lane_(lane) {
  redirectLogger("RelayGrants");
}

void RelayGrantCoordinator::SetStore(IRelayGrantStore* store) {
  std::lock_guard lock(mutex_);
  store_ = store;
}

void RelayGrantCoordinator::SetOnGrantReceived(std::function<void(const std::string&)> on_received) {
  std::lock_guard lock(mutex_);
  on_grant_received_ = std::move(on_received);
}

void RelayGrantCoordinator::SetGrantedPeer(std::function<bool(const std::string&)> granted) {
  std::lock_guard lock(mutex_);
  granted_ = std::move(granted);
}

int64_t RelayGrantCoordinator::Now() const {
  return now_ ? now_() : util::NowUnixMs();
}

void RelayGrantCoordinator::SetBlockedPeer(std::function<bool(const std::string&)> blocked) {
  std::lock_guard lock(mutex_);
  blocked_ = std::move(blocked);
}

bool RelayGrantCoordinator::Blocked(const std::string& account) const {
  std::function<bool(const std::string&)> blocked;
  {
    std::lock_guard lock(mutex_);
    blocked = blocked_;
  }
  return blocked && blocked(account);
}

bool RelayGrantCoordinator::Granted(const std::string& account) const {
  std::function<bool(const std::string&)> granted;
  {
    std::lock_guard lock(mutex_);
    granted = granted_;
  }
  return granted && !account.empty() && granted(account);
}

std::string RelayGrantCoordinator::OwnAccount() const {
  auto identity = identity_.Get();
  return identity ? identity->account_id : std::string();
}

bool RelayGrantCoordinator::Due(std::unordered_map<std::string, int64_t>& when, const std::string& key,
                                const int64_t every_ms) {
  const int64_t now = Now();
  std::lock_guard lock(mutex_);
  auto [it, added] = when.try_emplace(key, now);
  if (!added && now - it->second < every_ms) {
    return false;
  }
  it->second = now;
  return true;
}

RelaySendHooks RelayGrantCoordinator::SendHooks() {
  RelaySendHooks hooks;
  hooks.grant_for = [this](const std::string& recipient) { return GrantFor(recipient); };
  hooks.grant_refused = [this](const std::string& recipient) { GrantRefused(recipient); };
  hooks.sender_public_key = [this]() {
    auto identity = identity_.Get();
    return identity ? identity->account_signing_public_key_b64 : std::string();
  };
  return hooks;
}

SystemControlFamily RelayGrantCoordinator::Family() {
  return relay_grant::MakeFamily(
      [this](const relay_grant::Notice& notice, const std::string& sender) { return OnNotice(notice, sender); });
}

std::optional<RelayGrant> RelayGrantCoordinator::IssueTo(const std::string& own, const std::string& account) {
  IRelayGrantStore* store = nullptr;
  {
    std::lock_guard lock(mutex_);
    store = store_;
  }
  if (!store) {
    return std::nullopt;
  }
  auto grant = IssueRelayGrant(own, account, Now(),
                               [this](const std::vector<uint8_t>& bytes) { return identity_.SignBytes(bytes); });
  if (!grant) {
    log().warning << "relay grant to " << account << " not signed: " << grant.error().message;
    return std::nullopt;
  }
  if (auto kept = store->PutIssued(IRelayGrantStore::Issued{.grant = *grant}); !kept) {
    log().warning << "relay grant to " << account << " not kept: " << kept.error().message;
    return std::nullopt;
  }
  return grant.value();
}

void RelayGrantCoordinator::SendGrants(std::vector<RelayGrant> grants) {
  std::vector<ControlSend> sends;
  sends.reserve(grants.size());
  for (const RelayGrant& grant : grants) {
    auto notice = relay_grant::BuildGrant(grant);
    if (!notice) {
      continue;
    }
    sends.push_back(ControlSend{.peer_identity = grant.sender_account_id, .control = std::move(*notice)});
  }
  if (sends.empty()) {
    return;
  }
  log().info << "relay grants out: " << sends.size();
  // Not live signaling: a lower lane, not to crowd a call's.
  lane_.SendToMany(std::move(sends), {}, WorkerLane::Normal);
}

void RelayGrantCoordinator::EnsureGranted(const std::vector<std::string>& accounts) {
  IssueWhereDue(accounts, [this](const std::string& account) { return Granted(account); });
}

void RelayGrantCoordinator::GrantCallPeers(const std::vector<std::string>& accounts) {
  IssueWhereDue(accounts,
                [this](const std::string& account) { return IsAccountIdentityValue(account) && !Blocked(account); });
}

void RelayGrantCoordinator::IssueWhereDue(const std::vector<std::string>& accounts,
                                          const std::function<bool(const std::string&)>& wanted) {
  IRelayGrantStore* store = nullptr;
  {
    std::lock_guard lock(mutex_);
    store = store_;
  }
  const std::string own = OwnAccount();
  if (!store || own.empty()) {
    return;
  }
  const int64_t now = Now();
  std::vector<RelayGrant> out;
  for (const std::string& account : accounts) {
    if (account == own || !wanted(account)) {
      continue;
    }
    auto issued = store->IssuedTo(account);
    if (issued && *issued && !RelayGrantDueForRenewal((*issued)->grant.expires_at, now)) {
      continue;
    }
    if (auto grant = IssueTo(own, account)) {
      out.push_back(std::move(*grant));
    }
  }
  SendGrants(std::move(out));
}

void RelayGrantCoordinator::Stop(const std::string& account) {
  IRelayGrantStore* store = nullptr;
  {
    std::lock_guard lock(mutex_);
    store = store_;
  }
  if (store) {
    (void)store->DropIssued(account);
  }
}

void RelayGrantCoordinator::NoteHeardFrom(const std::string& account) {
  if (!Granted(account) || !Due(heard_at_, account, kHeardEveryMs)) {
    return;
  }
  IRelayGrantStore* store = nullptr;
  {
    std::lock_guard lock(mutex_);
    store = store_;
  }
  const std::string own = OwnAccount();
  if (!store || own.empty() || account == own) {
    return;
  }
  // Ours to it: a missing or due one is issued; one it has not confirmed goes again, now that it is online.
  auto issued = store->IssuedTo(account);
  if (!issued || !*issued || RelayGrantDueForRenewal((*issued)->grant.expires_at, Now())) {
    EnsureGranted({account});
  } else if (!(*issued)->confirmed && Due(resent_at_, account, kResendEveryMs)) {
    SendGrants({(*issued)->grant});
  }
  // Its to us: asked for when we have held none we could use for a while (it may be on its way).
  auto received = store->Received(account);
  if (received && *received && RelayGrantUsable(**received, Now())) {
    return;
  }
  const int64_t now = Now();
  bool lacking_long = false;
  {
    std::lock_guard lock(mutex_);
    const auto [it, first] = lacking_since_.try_emplace(account, now);
    lacking_long = !first && now - it->second >= kAskAfterLackingMs;
  }
  if (lacking_long) {
    Ask(account, false);
  }
}

std::optional<RelayGrant> RelayGrantCoordinator::GrantFor(const std::string& recipient) {
  IRelayGrantStore* store = nullptr;
  {
    std::lock_guard lock(mutex_);
    store = store_;
  }
  if (!store || recipient.empty() || recipient == OwnAccount()) {
    return std::nullopt;
  }
  // No ask from here: a send is no sign the grantor is online, and the ask would be one more stranger's send
  // (NoteHeardFrom asks).
  auto received = store->Received(recipient);
  if (received && *received && RelayGrantUsable(**received, Now())) {
    return **received;
  }
  return std::nullopt;
}

void RelayGrantCoordinator::GrantRefused(const std::string& recipient) {
  IRelayGrantStore* store = nullptr;
  {
    std::lock_guard lock(mutex_);
    store = store_;
  }
  log().info << "relay refused the grant from " << recipient << "; asking for a new one";
  if (store) {
    (void)store->DropReceived(recipient);
  }
  Ask(recipient, true);
}

void RelayGrantCoordinator::Ask(const std::string& account, const bool refused) {
  if (!Due(asked_at_, account, refused ? kAskAfterRefusalMs : kAskEveryMs)) {
    return;
  }
  auto ask = relay_grant::BuildAsk();
  if (!ask) {
    return;
  }
  log().info << "asking " << account << " for a relay grant";
  lane_.SendToMany({ControlSend{.peer_identity = account, .control = std::move(*ask)}}, {}, WorkerLane::Normal);
}

Roe<void> RelayGrantCoordinator::OnNotice(const relay_grant::Notice& notice, const std::string& sender) {
  IRelayGrantStore* store = nullptr;
  {
    std::lock_guard lock(mutex_);
    store = store_;
  }
  const std::string own = OwnAccount();
  if (!store || own.empty() || sender == own) {
    return {};
  }
  switch (notice.kind) {
    case relay_grant::NoticeKind::Grant: {
      // Only the grantor sends its grant, and only to us; it must be its account's signature.
      const RelayGrant& grant = *notice.grant;
      auto key = peer_keys_.KeptSigning().Resolve("account", sender);
      if (!key) {
        return Error("relay grant: no key for " + sender);
      }
      if (auto checked = CheckRelayGrant(grant, sender, own, key->signing_public_key_b64, Now()); !checked) {
        return checked.error();
      }
      if (auto kept = store->PutReceived(grant); !kept) {
        return kept.error();
      }
      std::function<void(const std::string&)> on_received;
      {
        std::lock_guard lock(mutex_);
        lacking_since_.erase(sender);
        on_received = on_grant_received_;
      }
      if (auto ack = relay_grant::BuildAck(grant.issued_at)) {
        lane_.SendToMany({ControlSend{.peer_identity = sender, .control = std::move(*ack)}}, {}, WorkerLane::Normal);
      }
      if (on_received) {
        on_received(sender); // sends held back by its stranger quota go now, as a contact's
      }
      return {};
    }
    case relay_grant::NoticeKind::Ack: {
      auto issued = store->IssuedTo(sender);
      if (issued && *issued && (*issued)->grant.issued_at == notice.issued_at && !(*issued)->confirmed) {
        IRelayGrantStore::Issued confirmed = **issued;
        confirmed.confirmed = true;
        return store->PutIssued(confirmed);
      }
      return {};
    }
    case relay_grant::NoticeKind::Ask: {
      if (Blocked(sender) || !Due(resent_at_, sender, kResendOnAskMs)) {
        return {};
      }
      auto issued = store->IssuedTo(sender);
      const bool current = issued && *issued && (*issued)->grant.expires_at > Now();
      if (current && !RelayGrantDueForRenewal((*issued)->grant.expires_at, Now())) {
        SendGrants({(*issued)->grant}); // a granted peer's, or one a call gave: the same grant again
      } else if (Granted(sender)) {
        if (auto grant = IssueTo(own, sender)) {
          SendGrants({std::move(*grant)});
        }
      } else if (current) {
        SendGrants({(*issued)->grant}); // a call's grant is not renewed, only sent again while it lasts
      }
      return {};
    }
  }
  return {};
}

} // namespace pbr
