#include "feature/conversations/ContactRefreshWorkflow.h"

#include "common/directory/KeyCard.h"
#include "lib/net/RelayOrigins.h"
#include "lib/people/AccountCard.h"
#include "common/runtime/AppRuntime.h"
#include "lib/people/ContactIdentity.h"
#include "lib/people/DirectoryAnswers.h"
#include "lib/people/PeopleLookupFailure.h"

#include "common/Logger.h"
#include "common/Utilities.h"

#include <algorithm>
#include <set>
#include <utility>
#include <vector>

namespace pbr {

namespace {

logging::Logger Log() {
  return logging::getLogger("ContactRefreshWorkflow");
}

bool SameIcon(const std::optional<mesh::ProfileIconRef>& a, const std::optional<mesh::ProfileIconRef>& b) {
  const bool a_empty = !a || a->empty();
  const bool b_empty = !b || b->empty();
  if (a_empty || b_empty) {
    return a_empty == b_empty;
  }
  return a->url == b->url && a->blob_id == b->blob_id && a->kind == b->kind;
}

std::set<std::string> AsSet(const std::vector<std::string>& values) {
  return {values.begin(), values.end()};
}

bool NonEmpty(const std::optional<std::string>& value) {
  return value && !value->empty();
}

mesh::DirectoryEndpoint* FindEndpoint(Contact& contact, const std::string& peer_id) {
  for (mesh::DirectoryEndpoint& row : contact.remote.endpoints) {
    if (row.peer_id == peer_id) {
      return &row;
    }
  }
  return nullptr;
}

/** The card's devices become device rows (dials to them pass the address audience). */
void MergeCardDevices(Contact& contact, const ContactRefreshInputs& inputs, ContactRefreshResult& result) {
  if (!inputs.card_after) {
    return;
  }
  const PeerCardRecord& after = *inputs.card_after;
  if (!inputs.card_before || !SameKeys(inputs.card_before->keys, after.keys)) {
    result.keys_changed = true;
  }
  const std::vector<std::string> before_devices =
      inputs.card_before ? inputs.card_before->devices : std::vector<std::string>{};
  if (!after.devices.empty() && AsSet(after.devices) != AsSet(before_devices)) {
    result.devices_changed = true;
  }
  for (const std::string& peer_id : after.devices) {
    if (!peer_id.empty() && FindEndpoint(contact, peer_id) == nullptr) {
      contact.remote.endpoints.push_back(mesh::DirectoryEndpoint{.peer_id = peer_id});
      result.devices_changed = true;
    }
  }
}

/** The directory's rows: a device it lists is added, a known one takes its addresses. */
void MergeDirectoryEndpoints(Contact& contact, const mesh::DirectoryHit& hit, ContactRefreshResult& result) {
  for (const mesh::DirectoryEndpoint& offered : hit.endpoints) {
    if (offered.peer_id.empty()) {
      continue;
    }
    mesh::DirectoryEndpoint* row = FindEndpoint(contact, offered.peer_id);
    if (row == nullptr) {
      contact.remote.endpoints.push_back(offered);
      result.devices_changed = true;
      continue;
    }
    if (AsSet(row->multiaddrs) != AsSet(offered.multiaddrs)) {
      row->multiaddrs = offered.multiaddrs;
      result.other_changed = true;
    }
    row->updated_at = std::max(row->updated_at, offered.updated_at);
  }
}

/** The Account ID from an answer that verified (it is the relay's address too, multi-relay MR001). */
void MergeDirectoryIds(Contact& contact, const mesh::DirectoryHit& hit, ContactRefreshResult& result) {
  const auto offered_account = PrimaryAccountIdFromHit(hit);
  if (!ContactAccountId(contact) && offered_account) {
    for (mesh::ContactId& id : contact.remote.ids) {
      id.primary = false;
    }
    contact.remote.ids.insert(contact.remote.ids.begin(),
                              mesh::ContactId{mesh::ContactIdKind::Account, *offered_account, true});
    result.other_changed = true;
  }
}

/** The relay's row, when its answer verified: an unverified relay answer contributes nothing. */
const mesh::DirectoryHit* VerifiedHit(const ContactRefreshInputs& inputs) {
  return inputs.hit && inputs.hit_keys_verified ? &*inputs.hit : nullptr;
}

void MergeNickname(Contact& contact, const ContactRefreshInputs& inputs, ContactRefreshResult& result) {
  std::string nickname;
  const mesh::DirectoryHit* hit = VerifiedHit(inputs);
  if (inputs.card_fresh && inputs.card_after && !inputs.card_after->nickname.empty()) {
    nickname = inputs.card_after->nickname; // signed by the account, fetched now
  } else if (hit && !hit->nickname.empty()) {
    nickname = hit->nickname;
  }
  if (!nickname.empty() && nickname != contact.remote.nickname) {
    contact.remote.nickname = nickname;
    result.name_changed = true;
  }
}

} // namespace

Contact MergeContactRefresh(Contact contact, const ContactRefreshInputs& inputs, const int64_t now_ms,
                            ContactRefreshResult& result) {
  MergeCardDevices(contact, inputs, result);
  MergeNickname(contact, inputs, result);
  if (const mesh::DirectoryHit* hit = VerifiedHit(inputs)) {
    if (!SameIcon(contact.remote.icon, hit->icon)) {
      contact.remote.icon = hit->icon;
      result.photo_changed = true;
    }
    MergeDirectoryEndpoints(contact, *hit, result);
    MergeDirectoryIds(contact, *hit, result);
    contact.remote.fetched_at = now_ms;
  }
  SyncContactMirrors(contact);
  return contact;
}

const ContactRefreshResult::RelayAsk* ContactRefreshResult::AnsweredBy() const {
  for (const RelayAsk& ask : relays) {
    if (ask.outcome == RelayAsk::Outcome::Answered) {
      return &ask;
    }
  }
  return nullptr;
}

struct ContactRefreshWorkflow::Run {
  std::string contact_id;
  std::string account_id;
  std::optional<PeerCardRecord> card_before;
  std::optional<mesh::DirectoryHit> hit;
  bool hit_keys_verified = false;
  /** A relay's stored card was kept now (it verified): the kept card is the newest word on the name. */
  bool relay_card_kept = false;
  ContactRefreshResult result;
  std::function<void(ContactRefreshResult)> done;
};

ContactRefreshWorkflow::ContactRefreshWorkflow(ContactsStore& contacts, AccountKeyResolver& keys,
                                               ContactCardExchange& cards)
    : contacts_(contacts),
      keys_(keys),
      cards_(cards),
      post_blocking_([](std::function<void()> task) { AppRuntime::PostWorkerNormal(std::move(task)); }),
      post_ui_([](std::function<void()> task) { AppRuntime::PostUI(std::move(task)); }),
      lookup_(&LookupStoredAccount) {}

ContactRefreshWorkflow::~ContactRefreshWorkflow() = default;

void ContactRefreshWorkflow::SetPosts(Post blocking, Post ui) {
  post_blocking_ = std::move(blocking);
  post_ui_ = std::move(ui);
}

void ContactRefreshWorkflow::SetLookup(RelayLookup lookup) {
  std::lock_guard lock(mutex_);
  lookup_ = std::move(lookup);
}

void ContactRefreshWorkflow::SetOurRelays(std::vector<std::string> origins) {
  std::lock_guard lock(mutex_);
  our_relays_ = std::move(origins);
}

bool ContactRefreshWorkflow::Running(const std::string& contact_id) const {
  std::lock_guard lock(mutex_);
  return running_.count(contact_id) > 0;
}

bool ContactRefreshWorkflow::Start(const std::string& contact_id, std::function<void(ContactRefreshResult)> done) {
  auto found = contacts_.Get(contact_id);
  if (!found || !*found) {
    return false;
  }
  {
    std::lock_guard lock(mutex_);
    if (!running_.insert(contact_id).second) {
      return false; // pressed again while it runs
    }
  }
  const Contact& contact = **found;
  auto run = std::make_shared<Run>();
  run->contact_id = contact_id;
  run->account_id = ContactAccountId(contact).value_or("");
  run->done = std::move(done);
  if (!run->account_id.empty()) {
    if (auto kept = keys_.Kept(run->account_id); kept && *kept) {
      run->card_before = **kept;
    }
  }
  // First the contact's own devices (their card is the account's signed word), then the relays.
  cards_.AskDevices(contact, [this, alive = std::weak_ptr<int>(alive_), run](ContactCardExchange::DeviceAsk ask) {
    if (alive.expired()) {
      return;
    }
    using Devices = ContactRefreshResult::Devices;
    switch (ask) {
      case ContactCardExchange::DeviceAsk::NoDevice:
        run->result.devices = Devices::NotAsked;
        break;
      case ContactCardExchange::DeviceAsk::Kept:
        run->result.devices = Devices::Answered;
        break;
      case ContactCardExchange::DeviceAsk::NotReached:
        run->result.devices = Devices::NotReached;
        break;
    }
    post_blocking_([this, alive, run]() {
      if (!alive.expired()) {
        AskRelays(run);
      }
    });
  });
  return true;
}

void ContactRefreshWorkflow::AskRelays(const std::shared_ptr<Run>& run) {
  if (!run->account_id.empty()) { // relays know people by Account ID only (MR001)
    AskRelaysInOrder(*run);
  }
  post_ui_([this, alive = std::weak_ptr<int>(alive_), run]() {
    if (!alive.expired()) {
      Finish(run);
    }
  });
}

void ContactRefreshWorkflow::AskRelaysInOrder(Run& run) {
  using Whose = ContactRefreshResult::RelayAsk::Whose;
  // The contact's relays as its newest card says now (a device may just have given a newer list).
  std::vector<std::string> theirs;
  if (auto contact = contacts_.Get(run.contact_id); contact && *contact) {
    auto kept = keys_.Kept(run.account_id);
    theirs = ContactRelays(**contact, kept ? *kept : std::nullopt).origins;
  }
  for (const std::string& origin : theirs) {
    if (AskRelay(run, origin, Whose::Theirs)) {
      return;
    }
  }
  std::vector<std::string> ours;
  {
    std::lock_guard lock(mutex_);
    ours = our_relays_;
  }
  for (const std::string& origin : ours) {
    const bool asked = std::any_of(theirs.begin(), theirs.end(),
                                   [&origin](const std::string& known) { return SameRelay(known, origin); });
    if (!asked && AskRelay(run, origin, Whose::Ours)) {
      return;
    }
  }
}

bool ContactRefreshWorkflow::AskRelay(Run& run, const std::string& origin,
                                      const ContactRefreshResult::RelayAsk::Whose whose) {
  using Outcome = ContactRefreshResult::RelayAsk::Outcome;
  RelayLookup lookup;
  {
    std::lock_guard lock(mutex_);
    lookup = lookup_;
  }
  auto answer = lookup ? lookup(origin, run.account_id) : Roe<std::optional<StoredAccountRow>>(Error("no lookup"));
  Outcome outcome = Outcome::Answered;
  if (!answer) {
    outcome = Outcome::Unreachable;
    Log().info << "refresh: " << RelayOriginKey(origin) << " not reached: " << answer.error().message;
  } else if (!*answer) {
    outcome = Outcome::NotFound;
  } else if (!TakeRelayAnswer(run, std::move(**answer))) {
    outcome = Outcome::Rejected; // nothing of it used: the next relay is asked
  }
  run.result.relays.push_back({.origin = origin, .whose = whose, .outcome = outcome});
  Remember(run.account_id, origin, outcome);
  return outcome == Outcome::Answered;
}

bool ContactRefreshWorkflow::TakeRelayAnswer(Run& run, StoredAccountRow row) {
  const auto reject = [&run](const std::string& why) {
    run.result.relay_keys_rejected = true;
    Log().warning << "refresh: a relay's answer is not used: " << why;
    return false;
  };
  mesh::DirectoryHit& hit = row.hit;
  if (PrimaryAccountIdFromHit(hit) != run.account_id) {
    return reject("it is for another account"); // asked for one account, answered for another (or for none)
  }
  // Checked before anything is kept: an answer whose keys do not verify gives nothing, not even its card.
  const bool carries_keys = NonEmpty(hit.signing_public_key_b64) || NonEmpty(hit.kem_public_key_b64) ||
                            NonEmpty(hit.kem_binding_signature_b64);
  if (carries_keys && !DirectoryHitKeysVerify(hit)) {
    return reject("its keys do not verify against the Account ID");
  }
  const CardTaken card = row.card ? TakeRelayCard(run, *row.card) : CardTaken::Stale;
  if (card == CardTaken::Rejected) {
    return reject("its card does not verify");
  }
  if (carries_keys) {
    // Kept only when the signing key hashes to the Account ID and the KEM binding verifies (VerifyKeyCard).
    if (auto kept = keys_.AddDirectoryHit(hit); !kept) {
      return reject("its keys were not kept: " + kept.error().message);
    }
  } else if (card != CardTaken::Kept) {
    return reject("it carries no keys and no card that verifies");
  }
  run.relay_card_kept = run.relay_card_kept || card == CardTaken::Kept;
  run.hit_keys_verified = true;
  run.hit = std::move(hit);
  return true;
}

ContactRefreshWorkflow::CardTaken ContactRefreshWorkflow::TakeRelayCard(Run& run, const Object& card) {
  // The card the account registered there (multi-relay R2): kept when it verifies; the newest card wins.
  auto kept = keys_.AddCarriedCard(card, util::NowUnixMs(), "relay", run.account_id);
  if (kept) {
    return CardTaken::Kept;
  }
  auto parsed = AccountCardFromJson(card);
  if (parsed && parsed->keys.account_id == run.account_id && parsed->issued_at < keys_.CardFloor(run.account_id)) {
    return CardTaken::Stale; // older than one this account had: stale, not forged
  }
  Log().warning << "refresh: a relay's card does not verify: " << kept.error().message;
  return CardTaken::Rejected;
}

void ContactRefreshWorkflow::Remember(const std::string& account_id, const std::string& origin,
                                      const ContactRefreshResult::RelayAsk::Outcome outcome) {
  const std::string key = account_id + "|" + RelayOriginKey(origin);
  std::lock_guard lock(mutex_);
  if (reach_.insert_or_assign(key, outcome).second) {
    reach_order_.push_back(key);
  }
  while (reach_order_.size() > kMaxRemembered) {
    reach_.erase(reach_order_.front());
    reach_order_.pop_front();
  }
}

ContactRelayView ContactRefreshWorkflow::PreferredRelay(const Contact& contact) {
  ContactRelayView view;
  const std::string account_id = ContactAccountId(contact).value_or("");
  std::optional<PeerCardRecord> kept;
  if (!account_id.empty()) {
    if (auto found = keys_.Kept(account_id); found) {
      kept = *found;
    }
  }
  const ContactRelayList list = ContactRelays(contact, kept);
  if (list.origins.empty()) {
    view.list = list.from_card ? ContactRelayView::List::MeshOnly : ContactRelayView::List::NotKnown;
    return view;
  }
  view.list = ContactRelayView::List::Listed;
  view.origin = list.origins.front();
  using Outcome = ContactRefreshResult::RelayAsk::Outcome;
  std::lock_guard lock(mutex_);
  if (auto it = reach_.find(account_id + "|" + RelayOriginKey(view.origin)); it != reach_.end()) {
    // A rejected answer reads as not reachable: what it said cannot be relied on.
    view.reach = it->second == Outcome::Answered   ? ContactRelayView::Reach::Reachable
                 : it->second == Outcome::NotFound ? ContactRelayView::Reach::NotRegistered
                                                   : ContactRelayView::Reach::Unreachable;
  }
  return view;
}

void ContactRefreshWorkflow::Finish(const std::shared_ptr<Run>& run) {
  {
    std::lock_guard lock(mutex_);
    running_.erase(run->contact_id);
  }
  ContactRefreshResult& result = run->result;
  auto found = contacts_.Get(run->contact_id);
  if (!found || !*found) {
    run->done(std::move(result)); // removed meanwhile
    return;
  }
  const Contact& current = **found;
  ContactRefreshInputs inputs;
  inputs.card_before = run->card_before;
  inputs.card_fresh = result.devices == ContactRefreshResult::Devices::Answered || run->relay_card_kept;
  inputs.hit = run->hit;
  inputs.hit_keys_verified = run->hit_keys_verified;
  std::string account_id = ContactAccountId(current).value_or("");
  if (account_id.empty() && run->hit && run->hit_keys_verified) {
    account_id = PrimaryAccountIdFromHit(*run->hit).value_or("");
  }
  if (!account_id.empty()) {
    if (auto kept = keys_.Kept(account_id); kept && *kept) {
      inputs.card_after = **kept;
    }
  }
  Contact merged = MergeContactRefresh(current, inputs, util::NowUnixMs(), result);
  result.contact = current;
  if (result.Changed() || run->hit) {
    if (auto stored = contacts_.Upsert(merged); stored) {
      result.contact = std::move(stored.value());
    } else {
      Log().warning << "refresh: not saved: " << stored.error().message;
      result.name_changed = result.photo_changed = result.devices_changed = result.other_changed = false;
    }
  }
  run->done(std::move(result));
}

} // namespace pbr
