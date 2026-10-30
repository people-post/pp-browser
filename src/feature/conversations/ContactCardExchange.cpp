#include "feature/conversations/ContactCardExchange.h"

#include "common/ValueJson.h"
#include "common/directory/DirectoryJson.h"
#include "lib/messaging/AccountCard.h"
#include "foundation/runtime/AppRuntime.h"
#include "common/Utilities.h"

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

} // namespace

ContactCardExchange::ContactCardExchange(ContactsStore& contacts, AccountKeyResolver& keys)
    : contacts_(contacts), keys_(keys) {
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
  fields.relay_user_id = IdOfKind(contact, mesh::ContactIdKind::RelayUser);
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
    return **existing;  // known already: its keys are refreshed above, its route stays its own
  }
  mesh::DirectoryHit hit;
  hit.hit_id = account_id;
  hit.account_id = account_id;
  hit.display_name = card.display_name;
  hit.nickname = kept.nickname.empty() ? card.display_name : kept.nickname;
  hit.ids = {{mesh::ContactIdKind::Account, account_id, true}};
  if (!card.relay_user_id.empty()) {
    hit.ids.push_back({mesh::ContactIdKind::RelayUser, card.relay_user_id, false});
  }
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
  fetch_card_(device, account_id,
              [this, devices = std::move(devices), account_id, done = std::move(done)](Roe<PeerCardRecord> kept) mutable {
                if (kept) {
                  done(true);
                  return;
                }
                FetchFromDevices(std::move(devices), account_id, std::move(done));
              });
}

void ContactCardExchange::AddFromLocator(const ContactLocator& locator, std::function<void(Roe<LocatorAdded>)> done) {
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
  auto contact = *existing ? Roe<Contact>(**existing) : contacts_.AddFromDirectoryHit(hit);
  if (!contact) {
    done(contact.error());
    return;
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
  FetchCard(locator.account_id, locator.devices, finish);
}

void ContactCardExchange::FetchCard(const std::string& account_id, std::vector<std::string> devices,
                                    std::function<void(bool kept)> done) {
  FetchFromDevices(std::move(devices), account_id, [this, account_id, done](bool kept) {
    if (kept) {
      done(true);
      return;
    }
    // No listed device answered: the directory, when there is one, has the keys too (an HTTP call: off
    // the I/O thread this callback may be on).
    auto task = [this, account_id, done]() {
      auto resolved = keys_.Resolve(account_id);
      if (!resolved) {
        log().info << "no card for " << account_id << " from its devices or the directory: "
                   << resolved.error().message;
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
  std::vector<std::string> devices;
  for (const mesh::ContactId& id : contact.remote.ids) {
    if (id.kind == mesh::ContactIdKind::PeerId && !id.value.empty()) {
      devices.push_back(id.value);
    }
  }
  log().info << "asking again for the card of " << account_id << " (" << devices.size() << " device(s))";
  FetchCard(account_id, std::move(devices), std::move(done));
}

} // namespace pbr
