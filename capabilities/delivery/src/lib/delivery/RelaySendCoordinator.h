#pragma once

#include "lib/delivery/RelayClients.h"
#include "lib/delivery/RelayHealth.h"
#include "common/Module.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <condition_variable>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/** Where a recipient's mailboxes are (multi-relay R2): its relays, preferred first, as its newest card lists them. */
struct RelayRoutes {
  /** The recipient's relays from the card we keep for it; empty: none known (our preferred relay carries it). */
  std::function<std::vector<std::string>(const std::string& recipient_account_id)> relays_of;
  /**
   * Fetch the recipient's newest card and give its relays: asked when every relay tried said the recipient is not
   * registered there (our copy of its list is stale). Blocking and bounded: a worker's.
   */
  std::function<std::vector<std::string>(const std::string& recipient_account_id)> refresh_relays_of;
};

/**
 * Sends into a recipient's relays in its list order (multi-relay R2; docs/contracts/SERVICE_ENDPOINTS.md § Sending
 * by the list): the relays its kept card lists, preferred first — none known: our preferred relay; to our own
 * account (a copy for our other devices): our relays. A relay that just failed (RelayHealth) goes after the healthy
 * ones. The next relay is tried on a transport failure or a 5xx; any other refusal (a price, a quota, a rate limit, a
 * bad request) is the answer. `recipient_not_registered` on every relay tried asks for the recipient's newest card
 * (at most once per kRefreshAgainAfterMs per recipient; a send meanwhile waits for that fetch, kRefreshWaitMs at most)
 * and tries its list once more when it names other relays. Our relays send as
 * registered senders; another relay gets a client of its own (made by the factory, at most kMaxForeignClients), where
 * the relay asks for our key (HttpRelayClient). Grants are per recipient, the same on every relay.
 *
 * The inbox calls (poll, ack, clear, stream history) are the preferred relay's. Thread-safe; a send blocks (HTTP).
 */
class RelaySendCoordinator : public IRelayClient, public Module {
public:
  struct OurRelay {
    std::string origin;
    std::shared_ptr<IRelayClient> client;
  };
  using ForeignClientFactory = std::function<std::shared_ptr<IRelayClient>(const std::string& origin)>;

  static constexpr size_t kMaxForeignClients = 16;
  static constexpr int64_t kRefreshAgainAfterMs = 60 * 1000;
  /** How long a send waits for another's fetch of the same recipient's card. */
  static constexpr int64_t kRefreshWaitMs = 12 * 1000;

  RelaySendCoordinator();

  /** Ours, preferred first; empty: no relay (every send fails). */
  void SetOurRelays(std::vector<OurRelay> ours);
  /** Clients for relays not ours; the ones made so far are dropped (their signer or hooks changed). */
  void SetForeignClients(ForeignClientFactory factory);
  void SetRoutes(RelayRoutes routes);
  /** Tests: the clock (Unix ms). */
  void SetClock(std::function<int64_t()> now_ms) { now_ms_ = std::move(now_ms); }
  RelayHealth& Health() { return health_; }

  Roe<void> Send(const RelayEnvelope& envelope) override;
  Roe<RelayPollResult> PollInbox(const std::string& requester_account_id, const std::string& cursor) override;
  Roe<RelayDeleteResult> AckInbox(const std::string& requester_account_id, const std::string& cursor) override;
  Roe<RelayDeleteResult> ClearInbox(const std::string& requester_account_id,
                                    const std::string& before_created_at) override;
  Roe<ChatHistoryResponse> FetchChatHistory(const ChatHistoryRequest& request) override;

private:
  /** How one pass along a list ended. */
  struct Pass {
    Roe<void> result;
    /** Every relay tried said `recipient_not_registered` (and one was tried). */
    bool not_registered_everywhere = false;
  };

  std::vector<std::string> ListFor(const std::string& recipient, bool to_self);
  Pass SendAlong(const std::vector<std::string>& origins, const RelayEnvelope& envelope);
  std::shared_ptr<IRelayClient> ClientFor(const std::string& origin);
  std::shared_ptr<IRelayClient> Preferred() const;
  std::vector<std::string> OurOrigins() const;
  /**
   * The recipient's list after its newest card was asked for: fetched now, or by a send just before (waited for while
   * it runs). The fallback (our preferred relay) when it lists none.
   */
  std::vector<std::string> NewestListOf(const std::string& recipient);
  int64_t Now() const;

  mutable std::mutex mutex_;
  std::vector<OurRelay> ours_;
  ForeignClientFactory factory_;
  /** By RelayOriginKey. */
  std::unordered_map<std::string, std::shared_ptr<IRelayClient>> foreign_;
  RelayRoutes routes_;
  std::unordered_map<std::string, int64_t> refreshed_at_;
  /** Recipients whose card a send is fetching now; `refreshed_` wakes the sends that wait for it. */
  std::set<std::string> refreshing_;
  std::condition_variable refreshed_;
  RelayHealth health_;
  std::function<int64_t()> now_ms_;
};

} // namespace pbr
