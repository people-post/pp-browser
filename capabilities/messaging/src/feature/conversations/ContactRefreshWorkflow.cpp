#include "feature/conversations/ContactRefreshWorkflow.h"

#include "common/directory/KeyCard.h"
#include "common/runtime/AppRuntime.h"
#include "lib/people/ContactIdentity.h"
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

/** The Account ID from an answer whose keys verified (it is the relay's address too, multi-relay MR001). */
void MergeDirectoryIds(Contact& contact, const ContactRefreshInputs& inputs, ContactRefreshResult& result) {
  const mesh::DirectoryHit& hit = *inputs.hit;
  const auto offered_account = PrimaryAccountIdFromHit(hit);
  if (!ContactAccountId(contact) && offered_account && inputs.hit_keys_verified) {
    for (mesh::ContactId& id : contact.remote.ids) {
      id.primary = false;
    }
    contact.remote.ids.insert(contact.remote.ids.begin(),
                              mesh::ContactId{mesh::ContactIdKind::Account, *offered_account, true});
    result.other_changed = true;
  }
}

void MergeNickname(Contact& contact, const ContactRefreshInputs& inputs, ContactRefreshResult& result) {
  std::string nickname;
  if (inputs.card_fresh && inputs.card_after && !inputs.card_after->nickname.empty()) {
    nickname = inputs.card_after->nickname; // signed by the account, fetched now
  } else if (inputs.hit && !inputs.hit->nickname.empty()) {
    nickname = inputs.hit->nickname;
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
  if (inputs.hit) {
    if (!SameIcon(contact.remote.icon, inputs.hit->icon)) {
      contact.remote.icon = inputs.hit->icon;
      result.photo_changed = true;
    }
    MergeDirectoryEndpoints(contact, *inputs.hit, result);
    MergeDirectoryIds(contact, inputs, result);
    contact.remote.fetched_at = now_ms;
  }
  SyncContactMirrors(contact);
  return contact;
}

struct ContactRefreshWorkflow::Run {
  std::string contact_id;
  std::string account_id;
  std::optional<PeerCardRecord> card_before;
  std::optional<mesh::DirectoryHit> hit;
  bool hit_keys_verified = false;
  ContactRefreshResult result;
  std::function<void(ContactRefreshResult)> done;
};

ContactRefreshWorkflow::ContactRefreshWorkflow(ContactsStore& contacts, AccountKeyResolver& keys,
                                               mesh::IDirectoryClient& directory, ContactCardExchange& cards)
    : contacts_(contacts),
      keys_(keys),
      directory_(directory),
      cards_(cards),
      post_blocking_([](std::function<void()> task) { AppRuntime::PostWorkerNormal(std::move(task)); }),
      post_ui_([](std::function<void()> task) { AppRuntime::PostUI(std::move(task)); }) {}

ContactRefreshWorkflow::~ContactRefreshWorkflow() = default;

void ContactRefreshWorkflow::SetPosts(Post blocking, Post ui) {
  post_blocking_ = std::move(blocking);
  post_ui_ = std::move(ui);
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
  // First the contact's own devices (their card is the account's signed word), then our directory.
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
        AskDirectory(run);
      }
    });
  });
  return true;
}

void ContactRefreshWorkflow::AskDirectory(const std::shared_ptr<Run>& run) {
  std::optional<Roe<mesh::DirectoryHit>> answer;
  if (!run->account_id.empty()) {
    answer = directory_.LookupByAccount(run->account_id); // the directory knows people by Account ID only
  }
  using Directory = ContactRefreshResult::Directory;
  if (!answer) {
    run->result.directory = Directory::NotAsked;
  } else if (!*answer) {
    const bool not_found = ClassifyPeopleLookupError(answer->error()) == PeopleLookupFailure::NotFound;
    run->result.directory = not_found ? Directory::NotFound : Directory::Unreachable;
    Log().info << "refresh: directory not answered: " << answer->error().message;
  } else {
    TakeDirectoryAnswer(*run, std::move(answer->value()));
  }
  post_ui_([this, alive = std::weak_ptr<int>(alive_), run]() {
    if (!alive.expired()) {
      Finish(run);
    }
  });
}

void ContactRefreshWorkflow::TakeDirectoryAnswer(Run& run, mesh::DirectoryHit hit) {
  run.result.directory = ContactRefreshResult::Directory::Answered;
  const auto offered = PrimaryAccountIdFromHit(hit);
  if (!run.account_id.empty() && offered != run.account_id) {
    // Asked for one account, answered for another (or for none): nothing in it is this contact's.
    run.result.directory_keys_rejected = true;
    Log().warning << "refresh: the directory answered for another account; not used";
    return;
  }
  const bool carries_keys = NonEmpty(hit.signing_public_key_b64) || NonEmpty(hit.kem_public_key_b64) ||
                            NonEmpty(hit.kem_binding_signature_b64);
  if (carries_keys) {
    // Kept only when the signing key hashes to the Account ID and the KEM binding verifies (VerifyKeyCard).
    if (auto kept = keys_.AddDirectoryHit(hit); kept) {
      run.hit_keys_verified = true;
    } else {
      run.result.directory_keys_rejected = true;
      Log().warning << "refresh: the directory's keys do not verify; not used: " << kept.error().message;
    }
  }
  run.hit = std::move(hit);
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
  inputs.card_fresh = result.devices == ContactRefreshResult::Devices::Answered;
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
