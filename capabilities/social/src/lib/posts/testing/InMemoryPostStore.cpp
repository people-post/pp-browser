#include "lib/posts/testing/InMemoryPostStore.h"

#include <algorithm>

namespace pbr {

Roe<IPostStore::Added> InMemoryPostStore::AddStub(const StoredPostEntry& row) {
  std::lock_guard lock(mutex_);
  auto& place = entries_[{row.author, row.lane, row.seq}];
  if (auto held = place.find(row.entry_hash); held != place.end()) {
    // Already here. An unkept stub gains the bytes; a deleted entry stays deleted.
    StoredPostEntry& kept = held->second;
    if (!kept.signed_bytes && row.signed_bytes && kept.stub == PostStub::Unkept) {
      kept.signed_bytes = row.signed_bytes;
      kept.stub = PostStub::None;
    }
    return Added::Duplicate;
  }
  const bool fork = !place.empty();
  StoredPostEntry stored = row;
  stored.stub = row.signed_bytes ? PostStub::None : row.stub;
  place.emplace(row.entry_hash, std::move(stored));
  return fork ? Added::Fork : Added::New;
}

Roe<void> InMemoryPostStore::StubOut(const std::string& author, const PostLane& lane, uint64_t seq,
                                     const PostHash& entry_hash, PostStub why) {
  std::lock_guard lock(mutex_);
  if (auto place = entries_.find({author, lane, seq}); place != entries_.end()) {
    if (auto entry = place->second.find(entry_hash); entry != place->second.end()) {
      entry->second.signed_bytes.reset();
      entry->second.stub = why;
    }
  }
  return {};
}

Roe<std::vector<StoredPostEntry>> InMemoryPostStore::At(const std::string& author, const PostLane& lane,
                                                        uint64_t seq) const {
  std::lock_guard lock(mutex_);
  std::vector<StoredPostEntry> out;
  if (auto place = entries_.find({author, lane, seq}); place != entries_.end()) {
    for (const auto& [hash, entry] : place->second) {
      out.push_back(entry);
    }
  }
  return out;
}

Roe<std::vector<StoredPostEntry>> InMemoryPostStore::Range(const std::string& author, const PostLane& lane,
                                                           uint64_t from_seq, size_t limit) const {
  std::lock_guard lock(mutex_);
  const size_t cap = std::min(limit, kMaxPostPageEntries);
  std::vector<StoredPostEntry> out;
  for (auto place = entries_.lower_bound({author, lane, from_seq}); place != entries_.end() && out.size() < cap;
       ++place) {
    if (std::get<0>(place->first) != author || std::get<1>(place->first) != lane) {
      break;
    }
    for (const auto& [hash, entry] : place->second) {
      if (out.size() == cap) {
        break;
      }
      out.push_back(entry);
    }
  }
  return out;
}

Roe<std::vector<PostHeadLane>> InMemoryPostStore::LanesOf(const std::string& author) const {
  std::lock_guard lock(mutex_);
  std::map<PostLane, PostHeadLane> lanes; // by lane; places come in seq order, so the last is the highest
  for (const auto& [place, held] : entries_) {
    if (std::get<0>(place) != author || held.empty()) {
      continue;
    }
    lanes[std::get<1>(place)] = {
        .lane = std::get<1>(place), .seq = std::get<2>(place), .entry_hash = held.begin()->first};
  }
  std::vector<PostHeadLane> out;
  for (const auto& [lane, head] : lanes) {
    out.push_back(head);
  }
  return out;
}

Roe<bool> InMemoryPostStore::HasFork(const std::string& author) const {
  std::lock_guard lock(mutex_);
  return std::any_of(entries_.begin(), entries_.end(),
                     [&](const auto& entry) { return std::get<0>(entry.first) == author && entry.second.size() > 1; });
}

Roe<void> InMemoryPostStore::PutHead(const SignedPostHead& head, const PostHead& parsed) {
  std::lock_guard lock(mutex_);
  auto kept = heads_.find(parsed.author);
  if (kept == heads_.end() || parsed.issued_at > kept->second.issued_at) {
    heads_[parsed.author] = KeptHead{parsed.issued_at, head};
  }
  return {};
}

Roe<std::optional<SignedPostHead>> InMemoryPostStore::Head(const std::string& author) const {
  std::lock_guard lock(mutex_);
  auto kept = heads_.find(author);
  return kept == heads_.end() ? std::optional<SignedPostHead>() : std::optional<SignedPostHead>(kept->second.head);
}

Roe<std::optional<OwnPostLane>> InMemoryPostStore::OwnLane(const std::string& device) const {
  std::lock_guard lock(mutex_);
  auto lane = own_lanes_.find(device);
  return lane == own_lanes_.end() ? std::optional<OwnPostLane>() : std::optional<OwnPostLane>(lane->second);
}

Roe<void> InMemoryPostStore::PutOwnLane(const OwnPostLane& lane) {
  std::lock_guard lock(mutex_);
  own_lanes_[lane.device] = lane;
  return {};
}

} // namespace pbr
