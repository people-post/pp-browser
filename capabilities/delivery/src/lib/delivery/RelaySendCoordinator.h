#pragma once

#include "lib/delivery/RelayClients.h"
#include "lib/delivery/RelayHealth.h"
#include "lib/net/RelayPins.h"
#include "lib/people/AccountCard.h"
#include "common/Module.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <condition_variable>
#include <mutex>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * Where a recipient's mailboxes are (multi-relay R2): its relays, preferred first, as its newest card lists them, each
 * with the PeerId the card names (MR007: the audience a send into it is signed for; empty: none named, its pin).
 */
struct RelayRoutes {
  /** The recipient's relays from the card we keep for it; empty: none known (our preferred relay carries it). */
  std::function<std::vector<AccountCardRelay>(const std::string& recipient_account_id)> relays_of;
  /**
   * Fetch the recipient's newest card and give its relays: asked when every relay tried said the recipient is not
   * registered there, or refused the PeerId the card named (our copy of its list is stale). Blocking and bounded.
   */
  std::function<std::vector<AccountCardRelay>(const std::string& recipient_account_id)> refresh_relays_of;
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
 * Each send into a recipient's relay is signed for the PeerId its card names (MR007; docs/contracts/SERVICE_ENDPOINTS.md
 * § Sending by the list). A card entry our pins disagree with (SetCardRelayCheck: our pin for that origin is another
 * PeerId, or that PeerId is pinned to another origin) is skipped, logged once per entry; a relay that answers
 * `wrong_audience` is skipped for this send and counts like `recipient_not_registered` toward the newest-card fetch,
 * and while the newest card still names that PeerId it is skipped for that recipient for kAudienceRefusedMs. Neither
 * marks the relay unhealthy. A relay of ours whose identity changed (not accepted) is skipped alike.
 *
 * The inbox calls (poll, ack, clear, stream history) are the preferred relay's. Thread-safe; a send blocks (HTTP).
 */
class RelaySendCoordinator : public IRelayClient, public Module {
public:
  struct OurRelay {
    std::string origin;
    std::shared_ptr<IRelayClient> client;
  };
  /** A client for a relay not ours, signing for `peer_id` (empty: the relay's pin). */
  using ForeignClientFactory =
      std::function<std::shared_ptr<IRelayClient>(const std::string& origin, const std::string& peer_id)>;
  using CardRelayCheckFn = std::function<CardRelayCheck(const std::string& origin, const std::string& peer_id)>;

  static constexpr size_t kMaxForeignClients = 16;
  /** How long a relay that refused a card's PeerId is skipped for that recipient (MR007). */
  static constexpr int64_t kAudienceRefusedMs = 5 * 60 * 1000;
  static constexpr int64_t kRefreshAgainAfterMs = 60 * 1000;
  /** How long a send waits for another's fetch of the same recipient's card. */
  static constexpr int64_t kRefreshWaitMs = 12 * 1000;

  RelaySendCoordinator();

  /** Ours, preferred first; empty: no relay (every send fails). */
  void SetOurRelays(std::vector<OurRelay> ours);
  /** Clients for relays not ours; the ones made so far are dropped (their signer or hooks changed). */
  void SetForeignClients(ForeignClientFactory factory);
  void SetRoutes(RelayRoutes routes);
  /** How a card's `{origin, peer_id}` stands against our pins (RelayPins::CheckCardRelay); none: always usable. */
  void SetCardRelayCheck(CardRelayCheckFn check);
  /**
   * A send to `recipient` found its relay at `origin` unusable for the PeerId its card names (a pin mismatch, or the
   * relay answered `wrong_audience`) within kAudienceRefusedMs: the contact page's *identity doesn't match*.
   */
  bool AudienceMismatch(const std::string& recipient_account_id, const std::string& origin) const;
  /** Tests: the clock (Unix ms). */
  void SetClock(std::function<int64_t()> now_ms) { now_ms_ = std::move(now_ms); }
  RelayHealth& Health() { return health_; }
  /**
   * The client for a card entry: ours (by origin), else a foreign one the factory accepts, for the entry's PeerId;
   * null when none.
   */
  std::shared_ptr<IRelayClient> ClientFor(const AccountCardRelay& relay);

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
    /**
     * Every relay tried said `recipient_not_registered` or refused the card's PeerId (and one was tried): our copy of
     * the recipient's list is stale.
     */
    bool not_registered_everywhere = false;
  };
  /** How one relay's try ended, for the pass. */
  enum class Try { Sent, Answer, FailedItself, Stale, Skipped };

  std::vector<AccountCardRelay> ListFor(const std::string& recipient, bool to_self);
  Pass SendAlong(const std::vector<AccountCardRelay>& relays, const RelayEnvelope& envelope);
  /** A card entry our pins or a recent refusal rule out for `recipient` (logged once per entry). */
  bool Unusable(const std::string& recipient, const AccountCardRelay& relay);
  /** Sends into one relay; what came of it (`result` holds the send's answer). */
  Try TryRelay(const AccountCardRelay& relay, const RelayEnvelope& envelope, Roe<void>& result);
  std::shared_ptr<IRelayClient> Preferred() const;
  std::vector<AccountCardRelay> OurRelayEntries() const;
  /**
   * The recipient's list after its newest card was asked for: fetched now, or by a send just before (waited for while
   * it runs). The fallback (our preferred relay) when it lists none.
   */
  std::vector<AccountCardRelay> NewestListOf(const std::string& recipient);
  int64_t Now() const;

  mutable std::mutex mutex_;
  std::vector<OurRelay> ours_;
  ForeignClientFactory factory_;
  /** By RelayOriginKey and the PeerId signed for. */
  std::unordered_map<std::string, std::shared_ptr<IRelayClient>> foreign_;
  RelayRoutes routes_;
  CardRelayCheckFn card_relay_check_;
  /** A card entry found unusable for a recipient: when last, and whether the relay itself refused it. */
  struct Mismatch {
    int64_t at_ms = 0;
    bool refused = false;
  };
  /** By (recipient, origin key, PeerId); bounded. */
  std::map<std::tuple<std::string, std::string, std::string>, Mismatch> mismatched_;
  std::unordered_map<std::string, int64_t> refreshed_at_;
  /** Recipients whose card a send is fetching now; `refreshed_` wakes the sends that wait for it. */
  std::set<std::string> refreshing_;
  std::condition_variable refreshed_;
  RelayHealth health_;
  std::function<int64_t()> now_ms_;
};

} // namespace pbr
