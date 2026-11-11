#include "lib/people/LastGoodRelayStore.h"

#include "common/AtomicFileWrite.h"
#include "common/ValueJson.h"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <utility>
#include "common/PbrCompat.h"

namespace pbr {
namespace {

constexpr int64_t kSchemaVersion = 1;

} // namespace

LastGoodRelayStore::LastGoodRelayStore(std::string path) : path_(std::move(path)) {}

void LastGoodRelayStore::Load() {
  if (path_.empty()) {
    return;
  }
  std::ifstream in(path_, std::ios::binary);
  if (!in) {
    return;
  }
  const std::string body((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  auto root = TryParseObject(body);
  if (!root || root->getIf<int64_t>("schema_version").value_or(0) != kSchemaVersion) {
    return;
  }
  std::vector<std::string> relays;
  if (const Array* listed = root->getArray("relays")) {
    for (const Value& item : listed->elements) {
      auto relay = asString(item);
      if (relay && !relay->empty() && std::find(relays.begin(), relays.end(), *relay) == relays.end() &&
          relays.size() < kMaxRelays) {
        relays.push_back(*relay);
      }
    }
  }
  std::lock_guard lock(mu_);
  relays_ = std::move(relays);
}

bool LastGoodRelayStore::Note(const std::string& relay_peer_id) {
  if (relay_peer_id.empty()) {
    return false;
  }
  std::lock_guard lock(mu_);
  if (!relays_.empty() && relays_.front() == relay_peer_id) {
    return false;
  }
  std::erase(relays_, relay_peer_id);
  relays_.insert(relays_.begin(), relay_peer_id);
  if (relays_.size() > kMaxRelays) {
    relays_.resize(kMaxRelays);
  }
  return true;
}

void LastGoodRelayStore::Save() const {
  if (path_.empty()) {
    return;
  }
  std::lock_guard save(save_mu_);
  std::vector<Value> relays;
  for (const std::string& relay : Relays()) {
    relays.emplace_back(relay);
  }
  Object root;
  root.set("schema_version", kSchemaVersion);
  root.set("relays", ArrayValue(std::move(relays)));
  // A hint that is learned again on the next circuit: replaced atomically, not synced.
  (void)AtomicFileWrite::Write(path_, DumpJson(root, 2), AtomicFileWrite::Sync::None);
}

std::vector<std::string> LastGoodRelayStore::Relays() const {
  std::lock_guard lock(mu_);
  return relays_;
}

} // namespace pbr
