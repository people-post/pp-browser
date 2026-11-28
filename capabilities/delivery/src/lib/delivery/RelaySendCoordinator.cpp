#include "lib/delivery/RelaySendCoordinator.h"

#include "common/Utilities.h"
#include "foundation/error/AppError.h"
#include "lib/net/certificate/RelayOrigins.h"

#include <algorithm>
#include <chrono>
#include <utility>

namespace pbr {

namespace {

/** What an entry names its relay by: its relay id (a card's or a locator's; "" for one of ours, by origin). */
std::string EntryName(const AccountCardRelay& relay) {
  return relay.relay_id.ToString();
}

bool IsNetworkCode(const Error& failure, const Err::Network code) {
  return AppError::CategoryOf(failure) == ErrorCategory::Network && static_cast<Err::Network>(failure.code) == code;
}

/**
 * The relay could not be reached or failed itself (a transport failure, a timeout, a 5xx): the next relay is tried
 * and this one is remembered as failed. A refusal the relay made on purpose is the send's answer.
 */
bool RelayFailedItself(const Error& failure) {
  return AppError::CategoryOf(failure) == ErrorCategory::Network &&
         !IsNetworkCode(failure, Err::Network::HttpRejected) &&
         !IsNetworkCode(failure, Err::Network::RecipientNotRegistered) &&
         !IsNetworkCode(failure, Err::Network::PaymentRequired);
}

bool IsAuthCode(const Error& failure, const Err::Auth code) {
  return AppError::CategoryOf(failure) == ErrorCategory::Auth && static_cast<Err::Auth>(failure.code) == code;
}

Error NoRelay() {
  return AppError::Config(Err::Config::MissingKey, "Relay base_url not configured");
}

constexpr size_t kMaxMismatches = 256;

} // namespace

RelaySendCoordinator::RelaySendCoordinator() {
  redirectLogger("RelaySendCoordinator");
}

void RelaySendCoordinator::SetOurRelays(std::vector<OurRelay> ours) {
  std::lock_guard lock(mutex_);
  ours_ = std::move(ours);
}

void RelaySendCoordinator::SetForeignClients(ForeignClientFactory factory) {
  std::lock_guard lock(mutex_);
  factory_ = std::move(factory);
  foreign_.clear();
}

void RelaySendCoordinator::SetRoutes(RelayRoutes routes) {
  std::lock_guard lock(mutex_);
  routes_ = std::move(routes);
}

void RelaySendCoordinator::SetCardRelayCheck(CardRelayCheckFn check) {
  std::lock_guard lock(mutex_);
  card_relay_check_ = std::move(check);
}

bool RelaySendCoordinator::AudienceMismatch(const std::string& recipient_account_id, const std::string& origin) const {
  const std::string key = RelayOriginKey(origin);
  const int64_t now_ms = Now();
  std::lock_guard lock(mutex_);
  return std::any_of(mismatched_.begin(), mismatched_.end(), [&](const auto& entry) {
    return std::get<0>(entry.first) == recipient_account_id && std::get<1>(entry.first) == key &&
           now_ms - entry.second.at_ms < kAudienceRefusedMs;
  });
}

int64_t RelaySendCoordinator::Now() const {
  return now_ms_ ? now_ms_() : util::NowUnixMs();
}

std::shared_ptr<IRelayClient> RelaySendCoordinator::Preferred() const {
  std::lock_guard lock(mutex_);
  return ours_.empty() ? nullptr : ours_.front().client;
}

std::vector<AccountCardRelay> RelaySendCoordinator::OurRelayEntries() const {
  std::lock_guard lock(mutex_);
  std::vector<AccountCardRelay> relays;
  for (const OurRelay& relay : ours_) {
    relays.push_back(AccountCardRelay{.origin = relay.origin}); // signed for by its pin
  }
  return relays;
}

std::vector<AccountCardRelay> RelaySendCoordinator::ListFor(const std::string& recipient, const bool to_self) {
  if (to_self) {
    return OurRelayEntries(); // our other devices poll every relay of ours
  }
  std::function<std::vector<AccountCardRelay>(const std::string&)> relays_of;
  {
    std::lock_guard lock(mutex_);
    relays_of = routes_.relays_of;
  }
  std::vector<AccountCardRelay> listed = relays_of ? relays_of(recipient) : std::vector<AccountCardRelay>{};
  if (!listed.empty()) {
    return listed;
  }
  std::vector<AccountCardRelay> ours = OurRelayEntries(); // no card (or an empty list): our preferred relay
  ours.resize(std::min<size_t>(ours.size(), 1));
  return ours;
}

std::shared_ptr<IRelayClient> RelaySendCoordinator::ClientFor(const AccountCardRelay& relay) {
  std::lock_guard lock(mutex_);
  for (const OurRelay& ours : ours_) {
    if (SameRelay(ours.origin, relay.origin)) {
      return ours.client; // its pin is the card's PeerId (Unusable ruled out any other)
    }
  }
  const std::string key = RelayOriginKey(relay.origin) + "|" + EntryName(relay);
  if (auto it = foreign_.find(key); it != foreign_.end()) {
    return it->second;
  }
  if (!factory_) {
    return nullptr;
  }
  std::shared_ptr<IRelayClient> client = factory_(relay);
  if (!client) {
    return nullptr;
  }
  if (foreign_.size() >= kMaxForeignClients) {
    foreign_.erase(foreign_.begin()); // any one: a client is only a URL and a signer, cheap to make again
  }
  foreign_[key] = client;
  return client;
}

bool RelaySendCoordinator::Unusable(const std::string& recipient, const AccountCardRelay& relay) {
  if (EntryName(relay).empty() || recipient.empty()) {
    return false; // signed for by its pin: the client says when that is not in force
  }
  CardRelayCheckFn check;
  const int64_t now_ms = Now();
  const auto entry = std::make_tuple(recipient, RelayOriginKey(relay.origin), EntryName(relay));
  {
    std::lock_guard lock(mutex_);
    check = card_relay_check_;
    if (auto it = mismatched_.find(entry);
        it != mismatched_.end() && it->second.refused && now_ms - it->second.at_ms < kAudienceRefusedMs) {
      return true; // it refused this PeerId a moment ago, and the newest card still names it
    }
  }
  if (!check || check(relay) == CardRelayCheck::Usable) {
    return false;
  }
  bool first = false;
  {
    std::lock_guard lock(mutex_);
    if (mismatched_.size() >= kMaxMismatches) {
      std::erase_if(mismatched_, [now_ms](const auto& e) { return now_ms - e.second.at_ms >= kAudienceRefusedMs; });
    }
    auto [it, inserted] = mismatched_.try_emplace(entry, Mismatch{});
    first = inserted;
    it->second.at_ms = now_ms;
  }
  if (first) {
    log().warning << "relay send: " << recipient << "'s card names " << RelayOriginKey(relay.origin) << " as "
                  << EntryName(relay) << ", which our pins disagree with; that relay is skipped for them";
  }
  return true;
}

RelaySendCoordinator::Try RelaySendCoordinator::TryRelay(const AccountCardRelay& relay, const RelayEnvelope& envelope,
                                                         Roe<void>& result) {
  std::shared_ptr<IRelayClient> client = ClientFor(relay);
  if (!client) {
    return Try::Skipped;
  }
  result = client->Send(envelope);
  if (result) {
    health_.NoteOk(relay.origin);
    return Try::Sent;
  }
  const Error& failure = result.error();
  if (IsAuthCode(failure, Err::Auth::WrongAudience)) {
    // The relay is not the one the card's PeerId names (it rotated, or the card is wrong): not unhealthy (MR007).
    const std::string recipient = envelope.recipient_account_id.value_or("");
    std::lock_guard lock(mutex_);
    mismatched_[std::make_tuple(recipient, RelayOriginKey(relay.origin), EntryName(relay))] =
        Mismatch{.at_ms = Now(), .refused = true};
    return Try::Stale;
  }
  if (IsAuthCode(failure, Err::Auth::RelayIdentityMismatch) || IsAuthCode(failure, Err::Auth::RelayIdentityTaken)) {
    return Try::Skipped; // nothing is signed for it until the person accepts its identity
  }
  if (RelayFailedItself(failure)) {
    health_.NoteFailed(relay.origin, Now());
    log().warning << "relay send: " << RelayOriginKey(relay.origin) << " failed (" << failure.message
                  << "); the next relay of the list";
    return Try::FailedItself;
  }
  if (IsNetworkCode(failure, Err::Network::RecipientNotRegistered)) {
    return Try::Stale;
  }
  return Try::Answer; // the relay's answer for this recipient (a price, a quota, a refusal)
}

RelaySendCoordinator::Pass RelaySendCoordinator::SendAlong(const std::vector<AccountCardRelay>& relays,
                                                           const RelayEnvelope& envelope) {
  Pass pass{.result = NoRelay()};
  const std::string recipient = envelope.recipient_account_id.value_or("");
  std::vector<std::string> origins;
  for (const AccountCardRelay& relay : relays) {
    origins.push_back(relay.origin);
  }
  size_t tried = 0;
  size_t stale = 0;
  bool failed_itself = false;
  for (const std::string& origin : health_.Order(origins, Now())) {
    const auto relay =
        std::find_if(relays.begin(), relays.end(), [&origin](const AccountCardRelay& r) { return r.origin == origin; });
    if (relay == relays.end()) {
      continue;
    }
    if (Unusable(recipient, *relay)) {
      if (tried == 0) {
        pass.result = AppError::Auth(Err::Auth::WrongAudience,
                                     "relay send: the recipient's relay does not match the identity its card names");
      }
      continue;
    }
    Roe<void> sent = Roe<void>{};
    const Try outcome = TryRelay(*relay, envelope, sent);
    if (outcome == Try::Sent || outcome == Try::Answer) {
      return Pass{.result = std::move(sent)};
    }
    if (outcome == Try::Skipped) {
      if (!failed_itself && tried == 0 && !sent) {
        pass.result = std::move(sent); // why nothing was sent, when nothing else is said
      }
      continue;
    }
    ++tried;
    if (outcome == Try::FailedItself) {
      failed_itself = true;
      pass.result = std::move(sent);
    } else if (++stale == tried) {
      pass.result = std::move(sent); // a relay that failed itself is the better answer: it may carry it later
    }
  }
  pass.not_registered_everywhere = tried > 0 && tried == stale;
  return pass;
}

std::vector<AccountCardRelay> RelaySendCoordinator::NewestListOf(const std::string& recipient) {
  std::function<std::vector<AccountCardRelay>(const std::string&)> refresh;
  {
    std::unique_lock lock(mutex_);
    refresh = routes_.refresh_relays_of;
    if (refreshing_.contains(recipient)) {
      // Another send is fetching its card: that fetch's list is this send's too.
      refreshed_.wait_for(lock, std::chrono::milliseconds(kRefreshWaitMs),
                          [this, &recipient] { return !refreshing_.contains(recipient); });
      refresh = nullptr;
    } else if (auto it = refreshed_at_.find(recipient);
               it != refreshed_at_.end() && Now() - it->second < kRefreshAgainAfterMs) {
      refresh = nullptr; // fetched a moment ago: the card we keep is that fetch's
    }
    if (refresh) {
      const int64_t now_ms = Now();
      if (refreshed_at_.size() >= 256) {
        std::erase_if(refreshed_at_,
                      [now_ms](const auto& entry) { return now_ms - entry.second >= kRefreshAgainAfterMs; });
      }
      refreshed_at_[recipient] = now_ms;
      refreshing_.insert(recipient);
    }
  }
  if (!refresh) {
    return ListFor(recipient, false);
  }
  std::vector<AccountCardRelay> fresh = refresh(recipient);
  {
    std::lock_guard lock(mutex_);
    refreshing_.erase(recipient);
  }
  refreshed_.notify_all();
  return fresh.empty() ? ListFor(recipient, false) : fresh;
}

Roe<void> RelaySendCoordinator::Send(const RelayEnvelope& envelope) {
  const std::string recipient = envelope.recipient_account_id.value_or("");
  const bool to_self = !recipient.empty() && recipient == envelope.sender_contact_id;
  const std::vector<AccountCardRelay> tried = ListFor(recipient, to_self);
  Pass first = SendAlong(tried, envelope);
  if (!first.not_registered_everywhere || to_self || recipient.empty()) {
    return std::move(first.result);
  }
  // Our copy of its list is stale (it moved): its newest card, and that list once more when it says other relays.
  const std::vector<AccountCardRelay> fresh = NewestListOf(recipient);
  if (fresh == tried) {
    return std::move(first.result);
  }
  log().info << "relay send: the recipient is on none of its known relays; trying its newest list (" << fresh.size()
             << ")";
  return std::move(SendAlong(fresh, envelope).result);
}

Roe<RelayPollResult> RelaySendCoordinator::PollInbox(const std::string& requester_account_id,
                                                     const std::string& cursor) {
  auto preferred = Preferred();
  return preferred ? preferred->PollInbox(requester_account_id, cursor) : Roe<RelayPollResult>(NoRelay());
}

Roe<RelayDeleteResult> RelaySendCoordinator::AckInbox(const std::string& requester_account_id,
                                                      const std::string& cursor) {
  auto preferred = Preferred();
  return preferred ? preferred->AckInbox(requester_account_id, cursor) : Roe<RelayDeleteResult>(NoRelay());
}

Roe<RelayDeleteResult> RelaySendCoordinator::ClearInbox(const std::string& requester_account_id,
                                                        const std::string& before_created_at) {
  auto preferred = Preferred();
  return preferred ? preferred->ClearInbox(requester_account_id, before_created_at) : Roe<RelayDeleteResult>(NoRelay());
}

Roe<ChatHistoryResponse> RelaySendCoordinator::FetchChatHistory(const ChatHistoryRequest& request) {
  auto preferred = Preferred();
  return preferred ? preferred->FetchChatHistory(request) : Roe<ChatHistoryResponse>(NoRelay());
}

} // namespace pbr
