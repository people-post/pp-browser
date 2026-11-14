#include "domain/delivery/RecipientRelays.h"

#include "common/Utilities.h"
#include "lib/delivery/RelayOrigins.h"
#include "lib/net/OrgBackendClientsImpl.h"
#include "lib/people/PeerCards.h"

#include <chrono>
#include <condition_variable>
#include <memory>
#include <utility>

namespace pbr {

namespace {

/** One device's answer, waited for with a deadline: a late answer lands here and is dropped. */
struct DeviceAnswer {
  std::mutex mutex;
  std::condition_variable answered;
  std::optional<bool> kept;
};

} // namespace

RecipientRelays::RecipientRelays(AccountKeyResolver& keys, DeviceAsk ask_device)
    : keys_(keys), ask_device_(std::move(ask_device)), fetch_stored_(&FetchStoredAccountCard) {
  redirectLogger("RecipientRelays");
}

void RecipientRelays::SetDirectories(std::vector<std::string> origins) {
  std::lock_guard lock(mutex_);
  directories_ = std::move(origins);
}

std::vector<std::string> RecipientRelays::Of(const std::string& account_id) {
  auto kept = keys_.Kept(account_id);
  if (!kept || !*kept) {
    return {};
  }
  std::vector<std::string> origins;
  for (const AccountCardRelay& relay : KeptRelays(**kept)) {
    origins.push_back(relay.origin);
  }
  return origins;
}

bool RecipientRelays::AskDevices(const std::string& account_id) {
  auto kept = keys_.Kept(account_id);
  if (!ask_device_ || !kept || !*kept || (*kept)->devices.empty()) {
    return false;
  }
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kDeviceAskTimeoutMs);
  for (const std::string& peer_id : (*kept)->devices) {
    auto answer = std::make_shared<DeviceAnswer>();
    ask_device_(peer_id, account_id, [answer](Roe<PeerCardRecord> card) {
      std::lock_guard lock(answer->mutex);
      answer->kept = static_cast<bool>(card); // Fetch keeps a card only once it checks out
      answer->answered.notify_all();
    });
    std::unique_lock lock(answer->mutex);
    if (!answer->answered.wait_until(lock, deadline, [&answer] { return answer->kept.has_value(); })) {
      return false; // out of time: the directories next
    }
    if (*answer->kept) {
      return true;
    }
  }
  return false;
}

void RecipientRelays::AskDirectories(const std::string& account_id) {
  std::vector<std::string> ours;
  {
    std::lock_guard lock(mutex_);
    ours = directories_;
  }
  for (const std::string& origin : ours) {
    auto stored = fetch_stored_ ? fetch_stored_(origin, account_id) : Roe<std::optional<Object>>(Error("no fetch"));
    if (!stored || !*stored) {
      continue;
    }
    // The card the account registered there: kept when it verifies and is newer than ours (newest wins).
    if (auto added = keys_.AddCarriedCard(**stored, util::NowUnixMs(), "directory", account_id); !added) {
      log().warning << "a stored card from " << RelayOriginKey(origin) << " not kept: " << added.error().message;
    }
  }
}

std::vector<std::string> RecipientRelays::Refresh(const std::string& account_id) {
  if (!AskDevices(account_id)) {
    AskDirectories(account_id);
  }
  return Of(account_id);
}

RelayRoutes RecipientRelays::Routes() {
  return RelayRoutes{
      .relays_of = [this](const std::string& account_id) { return Of(account_id); },
      .refresh_relays_of = [this](const std::string& account_id) { return Refresh(account_id); },
  };
}

} // namespace pbr
