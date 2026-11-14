#include "lib/delivery/RelaySendCoordinator.h"

#include "common/Utilities.h"
#include "foundation/error/AppError.h"
#include "lib/delivery/RelayOrigins.h"

#include <algorithm>
#include <chrono>
#include <utility>

namespace pbr {

namespace {

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

Error NoRelay() {
  return AppError::Config(Err::Config::MissingKey, "Relay base_url not configured");
}

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

int64_t RelaySendCoordinator::Now() const {
  return now_ms_ ? now_ms_() : util::NowUnixMs();
}

std::shared_ptr<IRelayClient> RelaySendCoordinator::Preferred() const {
  std::lock_guard lock(mutex_);
  return ours_.empty() ? nullptr : ours_.front().client;
}

std::vector<std::string> RelaySendCoordinator::OurOrigins() const {
  std::lock_guard lock(mutex_);
  std::vector<std::string> origins;
  for (const OurRelay& relay : ours_) {
    origins.push_back(relay.origin);
  }
  return origins;
}

std::vector<std::string> RelaySendCoordinator::ListFor(const std::string& recipient, const bool to_self) {
  if (to_self) {
    return OurOrigins(); // our other devices poll every relay of ours
  }
  std::function<std::vector<std::string>(const std::string&)> relays_of;
  {
    std::lock_guard lock(mutex_);
    relays_of = routes_.relays_of;
  }
  std::vector<std::string> listed = relays_of ? relays_of(recipient) : std::vector<std::string>{};
  if (!listed.empty()) {
    return listed;
  }
  std::vector<std::string> ours = OurOrigins(); // no card (or an empty list): today's way, our preferred relay
  ours.resize(std::min<size_t>(ours.size(), 1));
  return ours;
}

std::shared_ptr<IRelayClient> RelaySendCoordinator::ClientFor(const std::string& origin) {
  std::lock_guard lock(mutex_);
  for (const OurRelay& relay : ours_) {
    if (SameRelay(relay.origin, origin)) {
      return relay.client;
    }
  }
  const std::string key = RelayOriginKey(origin);
  if (auto it = foreign_.find(key); it != foreign_.end()) {
    return it->second;
  }
  if (!factory_) {
    return nullptr;
  }
  std::shared_ptr<IRelayClient> client = factory_(origin);
  if (!client) {
    return nullptr;
  }
  if (foreign_.size() >= kMaxForeignClients) {
    foreign_.erase(foreign_.begin()); // any one: a client is only a URL and a signer, cheap to make again
  }
  foreign_[key] = client;
  return client;
}

RelaySendCoordinator::Pass RelaySendCoordinator::SendAlong(const std::vector<std::string>& origins,
                                                           const RelayEnvelope& envelope) {
  Pass pass{.result = NoRelay()};
  size_t tried = 0;
  size_t not_registered = 0;
  for (const std::string& origin : health_.Order(origins, Now())) {
    std::shared_ptr<IRelayClient> client = ClientFor(origin);
    if (!client) {
      continue;
    }
    ++tried;
    Roe<void> sent = client->Send(envelope);
    if (sent) {
      health_.NoteOk(origin);
      return Pass{.result = std::move(sent)};
    }
    if (RelayFailedItself(sent.error())) {
      health_.NoteFailed(origin, Now());
      log().warning << "relay send: " << RelayOriginKey(origin) << " failed (" << sent.error().message
                    << "); the next relay of the list";
      pass.result = std::move(sent);
      continue;
    }
    if (IsNetworkCode(sent.error(), Err::Network::RecipientNotRegistered)) {
      ++not_registered;
      if (tried == not_registered) {
        pass.result = std::move(sent); // a relay that failed itself is the better answer: it may carry it later
      }
      continue;
    }
    return Pass{.result = std::move(sent)}; // the relay's answer for this recipient (a price, a quota, a refusal)
  }
  pass.not_registered_everywhere = tried > 0 && tried == not_registered;
  return pass;
}

std::vector<std::string> RelaySendCoordinator::NewestListOf(const std::string& recipient) {
  std::function<std::vector<std::string>(const std::string&)> refresh;
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
  std::vector<std::string> fresh = refresh(recipient);
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
  const std::vector<std::string> tried = ListFor(recipient, to_self);
  Pass first = SendAlong(tried, envelope);
  if (!first.not_registered_everywhere || to_self || recipient.empty()) {
    return std::move(first.result);
  }
  // Our copy of its list is stale (it moved): its newest card, and that list once more when it says other relays.
  const std::vector<std::string> fresh = NewestListOf(recipient);
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
