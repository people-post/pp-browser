#include "feature/conversations/ContactCardExchange.h"

#include "common/ValueJson.h"
#include "common/directory/DirectoryJson.h"
#include "lib/net/OrgBackendClientsImpl.h"
#include "lib/net/RelayOrigins.h"
#include "lib/people/AccountCard.h"
#include "common/runtime/AppRuntime.h"
#include "common/PbrCompat.h"
#include "common/Utilities.h"

#include <algorithm>
#include <optional>
#include <utility>

namespace pbr {

namespace {

std::string IdOfKind(const Contact& contact, mesh::ContactIdKind kind) {
  for (const mesh::ContactId& id : contact.remote.ids) {
    if (id.kind == kind && !id.value.empty()) {
      return id.value;
    }
  }
  return {};
}

std::vector<std::string> ContactDevices(const Contact& contact) {
  std::vector<std::string> devices;
  for (const mesh::ContactId& id : contact.remote.ids) {
    if (id.kind == mesh::ContactIdKind::PeerId && !id.value.empty()) {
      devices.push_back(id.value);
    }
  }
  return devices;
}

void AppendMissing(std::vector<std::string>& into, const std::vector<std::string>& more) {
  for (const std::string& value : more) {
    if (!value.empty() && std::find(into.begin(), into.end(), value) == into.end()) {
      into.push_back(value);
    }
  }
}

} // namespace

ContactCardExchange::ContactCardExchange(ContactsStore& contacts, AccountKeyResolver& keys)
    : contacts_(contacts), keys_(keys) {
  fetch_stored_ = [](const std::string& relay_origin, const std::string& account_id, const bool public_only) {
    return FetchStoredAccountCard(relay_origin, account_id, public_only);
  };
  redirectLogger("ContactCardExchange");
}

Roe<ChatContactCardFields> ContactCardExchange::CardFor(const std::string& contact_id) {
  auto found = contacts_.Get(contact_id);
  if (!found) {
    return found.error();
  }
  if (!*found) {
    return Error("Contact not found");
  }
  const Contact& contact = **found;
  const std::string account_id = IdOfKind(contact, mesh::ContactIdKind::Account);
  if (account_id.empty()) {
    return Error("Contact has no account to share");
  }
  auto kept = keys_.Kept(account_id);
  if (!kept || !*kept) {
    // Not met yet: the directory's keys, when there is one, are as good as any.
    auto resolved = keys_.Resolve(account_id);
    if (!resolved) {
      return Error("No card for this contact yet");
    }
    kept = std::optional<PeerCardRecord>(std::move(resolved.value()));
  }
  ChatContactCardFields fields;
  // The remark is the user's private note about this person: a card carries their own nickname only.
  fields.display_name = contact.remote.nickname;
  if (fields.display_name.empty()) {
    fields.display_name = account_id;
  }
  fields.card_json = DumpJson(CarriedCardJson(**kept));
  return fields;
}

Roe<Contact> ContactCardExchange::Add(const ChatContactCardFields& card, int64_t now_ms) {
  auto json = TryParseObject(card.card_json);
  if (!json) {
    return Error("contact card: not a card");
  }
  auto added = keys_.AddCarriedCard(*json, now_ms, "contact_card");
  if (!added) {
    return added.error();
  }
  const PeerCardRecord& kept = added.value();

  const std::string& account_id = kept.keys.account_id;
  auto existing = contacts_.FindByIdentity(account_id, mesh::ContactIdKind::Account);
  if (!existing) {
    return existing.error();
  }
  if (*existing) {
    return contacts_.ClearBlockOnAdd(**existing); // known already (its keys are refreshed above); adding ends a block
  }
  mesh::DirectoryHit hit;
  hit.hit_id = account_id;
  hit.account_id = account_id;
  hit.display_name = card.display_name;
  hit.nickname = kept.nickname.empty() ? card.display_name : kept.nickname;
  hit.ids = {{mesh::ContactIdKind::Account, account_id, true}};
  for (const std::string& peer_id : kept.devices) {
    hit.endpoints.push_back(mesh::DirectoryEndpoint{.peer_id = peer_id});
  }
  return contacts_.AddFromDirectoryHit(hit);
}

void ContactCardExchange::FetchFromDevices(std::vector<std::string> devices, const std::string& account_id,
                                           std::function<void(bool kept)> done) {
  if (devices.empty() || !fetch_card_) {
    done(false);
    return;
  }
  const std::string device = devices.front();
  devices.erase(devices.begin());
  fetch_card_(
      device, account_id,
      [this, devices = std::move(devices), account_id, done = std::move(done)](Roe<PeerCardRecord> kept) mutable {
        if (kept) {
          done(true);
          return;
        }
        FetchFromDevices(std::move(devices), account_id, std::move(done));
      });
}

void ContactCardExchange::AddFromLocator(const ContactLocator& locator, std::function<void(Roe<LocatorAdded>)> done,
                                         std::function<void(const Contact&)> stored) {
  mesh::DirectoryHit hit;
  hit.hit_id = locator.account_id;
  hit.account_id = locator.account_id;
  hit.nickname = locator.nickname;
  hit.display_name = locator.nickname;
  hit.ids = {{mesh::ContactIdKind::Account, locator.account_id, true}};
  for (const std::string& peer_id : locator.devices) {
    hit.endpoints.push_back(mesh::DirectoryEndpoint{.peer_id = peer_id});
  }
  auto existing = contacts_.FindByIdentity(locator.account_id, mesh::ContactIdKind::Account);
  if (!existing) {
    done(existing.error());
    return;
  }
  auto contact = *existing ? contacts_.ClearBlockOnAdd(**existing) : contacts_.AddFromDirectoryHit(hit);
  if (!contact) {
    done(contact.error());
    return;
  }
  const AccountCardRelay relay = UsableLocatorRelay(locator.relay);
  KeepLocatorRelay(*contact, relay);
  if (stored) {
    stored(*contact);
  }
  const auto finish = [this, account_id = locator.account_id, done](bool keys_known) {
    auto latest = contacts_.FindByIdentity(account_id, mesh::ContactIdKind::Account);
    if (!latest || !*latest) {
      done(Error("Contact vanished"));
      return;
    }
    done(LocatorAdded{.contact = **latest, .keys_known = keys_known});
  };
  if (auto kept = keys_.Kept(locator.account_id); kept && *kept) {
    finish(true);
    return;
  }
  FetchCard(locator.account_id, locator.devices, relay.origin, finish);
}

AccountCardRelay ContactCardExchange::UsableLocatorRelay(const AccountCardRelay& relay) {
  if (relay.origin.empty() ||
      IsPublicRelayOrigin(relay.origin, local_relays_allowed_.load(std::memory_order_relaxed))) {
    return relay;
  }
  // A QR code names a relay on this device or a private network: not a relay a release build writes to or asks.
  log().warning << "the locator's relay is not on a public host; left out";
  return {};
}

void ContactCardExchange::KeepLocatorRelay(Contact& contact, const AccountCardRelay& relay) {
  // Its relay list until its card is kept (ContactRelays, multi-relay R4): a first message can go to its mailbox,
  // signed for the PeerId the locator names (MR008).
  if (relay.origin.empty() || relay.peer_id.empty() ||
      (contact.remote.locator_relay == relay.origin && contact.remote.locator_relay_peer_id == relay.peer_id)) {
    return;
  }
  contact.remote.locator_relay = relay.origin;
  contact.remote.locator_relay_peer_id = relay.peer_id;
  if (auto stored = contacts_.Upsert(contact); stored) {
    contact = std::move(stored.value());
  } else {
    log().warning << "the locator's relay not kept: " << stored.error().message;
  }
}

bool ContactCardExchange::FetchFromRelay(const std::string& relay, const std::string& account_id) {
  if (relay.empty() || !fetch_stored_) {
    return false;
  }
  // The locator's relay is not ours: asked over https to a public address only, outside developer builds.
  auto stored = fetch_stored_(relay, account_id, !local_relays_allowed_.load(std::memory_order_relaxed));
  if (!stored || !*stored) {
    return false;
  }
  // The card the account registered there: kept only when it verifies against the Account ID.
  auto kept = keys_.AddCarriedCard(**stored, util::NowUnixMs(), "relay", account_id);
  if (!kept) {
    log().warning << "the locator's relay gave a card that does not hold: " << kept.error().message;
  }
  return static_cast<bool>(kept);
}

void ContactCardExchange::FetchCard(const std::string& account_id, std::vector<std::string> devices,
                                    const std::string& relay, std::function<void(bool kept)> done) {
  FetchFromDevices(std::move(devices), account_id, [this, account_id, relay, done](bool kept) {
    if (kept) {
      done(true);
      return;
    }
    // No listed device answered: the locator's relay keeps the card it registered with, and the directory, when there
    // is one, has the keys too (HTTP calls: off the I/O thread this callback may be on).
    auto task = [this, account_id, relay, done]() {
      if (FetchFromRelay(relay, account_id)) {
        done(true);
        return;
      }
      auto resolved = keys_.Resolve(account_id);
      if (!resolved) {
        log().info << "no card for " << account_id
                   << " from its devices or the directory: " << resolved.error().message;
      }
      done(static_cast<bool>(resolved));
    };
    post_blocking_ ? post_blocking_(std::move(task)) : AppRuntime::PostWorkerBackground(std::move(task));
  });
}

void ContactCardExchange::RefreshIfMissing(const Contact& contact, std::function<void(bool kept)> done) {
  const std::string account_id = IdOfKind(contact, mesh::ContactIdKind::Account);
  if (account_id.empty()) {
    done(false);
    return;
  }
  if (auto kept = keys_.Kept(account_id); kept && *kept) {
    done(false);
    return;
  }
  const int64_t now = util::NowUnixMs();
  {
    std::lock_guard lock(refetch_mutex_);
    int64_t& last = last_refetch_ms_[account_id];
    if (last != 0 && now - last < kRefetchAfterMs) {
      done(false);
      return;
    }
    last = now;
  }
  std::vector<std::string> devices = ContactDevices(contact);
  log().info << "asking again for the card of " << account_id << " (" << devices.size() << " device(s))";
  FetchCard(account_id, std::move(devices), contact.remote.locator_relay, std::move(done));
}

void ContactCardExchange::AskDevices(const Contact& contact, std::function<void(DeviceAsk)> done) {
  const std::string account_id = IdOfKind(contact, mesh::ContactIdKind::Account);
  // The account's own word on its devices first (the newest kept card), then the rows the contact has.
  std::vector<std::string> devices;
  if (!account_id.empty()) {
    if (auto kept = keys_.Kept(account_id); kept && *kept) {
      devices = (*kept)->devices;
    }
    AppendMissing(devices, ContactDevices(contact));
  }
  if (devices.empty() || !fetch_card_) {
    done(DeviceAsk::NoDevice);
    return;
  }
  const int64_t now = util::NowUnixMs();
  std::optional<DeviceAsk> just_asked;
  {
    std::lock_guard lock(refetch_mutex_);
    LastAsk& last = last_device_ask_[account_id];
    if (last.at_ms != 0 && now - last.at_ms < kAskDevicesAgainAfterMs) {
      just_asked = last.outcome;
    } else {
      last = LastAsk{.at_ms = now, .outcome = DeviceAsk::NotReached};
    }
  }
  if (just_asked) {
    done(*just_asked); // asked again at once: the devices were just asked
    return;
  }
  log().info << "refresh: asking " << devices.size() << " device(s) for the card of " << account_id;
  FetchFromDevices(std::move(devices), account_id, [this, account_id, done = std::move(done)](bool kept) {
    const DeviceAsk outcome = kept ? DeviceAsk::Kept : DeviceAsk::NotReached;
    {
      std::lock_guard lock(refetch_mutex_);
      last_device_ask_[account_id].outcome = outcome;
    }
    done(outcome);
  });
}

} // namespace pbr
