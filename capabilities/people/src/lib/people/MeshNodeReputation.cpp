#include "lib/people/MeshNodeReputation.h"

#include "common/AtomicFileWrite.h"
#include "common/ValueJson.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iterator>
#include "common/PbrCompat.h"

namespace pbr {
namespace {

constexpr int64_t kSchemaVersion = 1;

} // namespace

int64_t MeshNodeReputation::NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
      .count();
}

void MeshNodeReputation::SetPersistPath(std::string path) {
  std::lock_guard lock(mu_);
  path_ = std::move(path);
}

void MeshNodeReputation::Load() {
  std::string path;
  {
    std::lock_guard lock(mu_);
    path = path_;
  }
  if (path.empty()) {
    return;
  }
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return;
  }
  const std::string body((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  auto root = TryParseObject(body);
  if (!root || root->getIf<int64_t>("schema_version").value_or(0) != kSchemaVersion) {
    return;
  }
  auto read = [](const Object* object, Book& book) {
    if (!object) {
      return;
    }
    for (const auto& [key, value] : object->fields()) {
      const Object* row = asObject(value);
      if (!row || key.empty()) {
        continue;
      }
      Entry entry;
      entry.demoted_until_ms = row->getIf<int64_t>("demoted_until_ms").value_or(0);
      entry.last_strike_ms = row->getIf<int64_t>("last_strike_ms").value_or(0);
      entry.strikes = static_cast<int>(row->getIf<int64_t>("strikes").value_or(0));
      book[key] = entry;
    }
  };
  std::lock_guard lock(mu_);
  read(root->getObject("nodes"), nodes_);
  read(root->getObject("operators"), operators_);
}

bool MeshNodeReputation::Strike(Book& book, const std::string& key, const int64_t now_ms) {
  if (key.empty()) {
    return false;
  }
  auto it = book.find(key);
  if (it == book.end()) {
    if (book.size() >= kMaxEntries) {
      book.erase(std::min_element(book.begin(), book.end(), [](const auto& a, const auto& b) {
        return a.second.last_strike_ms < b.second.last_strike_ms;
      }));
    }
    it = book.emplace(key, Entry{}).first;
  }
  Entry& entry = it->second;
  if (now_ms < entry.demoted_until_ms) {
    return false; // already demoted: the same contradiction seen again is not a new strike
  }
  if (entry.last_strike_ms > 0 && now_ms - entry.last_strike_ms > kForgetAfterMs) {
    entry.strikes = 0;
  }
  ++entry.strikes;
  entry.last_strike_ms = now_ms;
  const int doublings = std::min(entry.strikes - 1, 8);
  entry.demoted_until_ms = now_ms + std::min(kFirstDemotionMs << doublings, kMaxDemotionMs);
  return true;
}

bool MeshNodeReputation::DemotedIn(const Book& book, const std::string& key, const int64_t now_ms) {
  if (key.empty()) {
    return false;
  }
  const auto it = book.find(key);
  return it != book.end() && now_ms < it->second.demoted_until_ms;
}

void MeshNodeReputation::NoteContradiction(const std::string& peer_id, const std::string& account_id,
                                           const int64_t now_ms) {
  std::lock_guard lock(mu_);
  const bool node = Strike(nodes_, peer_id, now_ms);
  const bool op = Strike(operators_, account_id, now_ms);
  if (node || op) {
    PersistLocked();
  }
}

bool MeshNodeReputation::IsDemoted(const std::string& peer_id, const std::string& account_id,
                                   const int64_t now_ms) const {
  std::lock_guard lock(mu_);
  return DemotedIn(nodes_, peer_id, now_ms) || DemotedIn(operators_, account_id, now_ms);
}

int MeshNodeReputation::Strikes(const std::string& peer_id) const {
  std::lock_guard lock(mu_);
  const auto it = nodes_.find(peer_id);
  return it == nodes_.end() ? 0 : it->second.strikes;
}

void MeshNodeReputation::PersistLocked() const {
  if (path_.empty()) {
    return;
  }
  auto write = [](const Book& book) {
    Object out;
    for (const auto& [key, entry] : book) {
      Object row;
      row.set("demoted_until_ms", entry.demoted_until_ms);
      row.set("last_strike_ms", entry.last_strike_ms);
      row.set("strikes", static_cast<int64_t>(entry.strikes));
      out.set(key, std::move(row));
    }
    return out;
  };
  Object root;
  root.set("schema_version", kSchemaVersion);
  root.set("nodes", write(nodes_));
  root.set("operators", write(operators_));
  (void)AtomicFileWrite::Write(path_, DumpJson(root, 2));
}

} // namespace pbr
