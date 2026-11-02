#pragma once

// Posts in memory, for social's tests and the tests above it (CR024). Never linked into the app or pp-node.

#include "lib/posts/IPostStore.h"

#include <map>
#include <mutex>
#include <string>
#include <tuple>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * IPostStore in memory, as the SQLite one: rivals at a place kept side by side (a fork), an unkept stub gaining its
 * bytes when the entry comes, the newest head per author, pages capped at kMaxPostPageEntries.
 */
class InMemoryPostStore : public IPostStore {
public:
  Roe<Added> AddStub(const StoredPostEntry& row) override;
  Roe<void> StubOut(const std::string& author, const PostLane& lane, uint64_t seq, const PostHash& entry_hash,
                    PostStub why) override;
  Roe<std::vector<StoredPostEntry>> At(const std::string& author, const PostLane& lane, uint64_t seq) const override;
  Roe<std::vector<StoredPostEntry>> Range(const std::string& author, const PostLane& lane, uint64_t from_seq,
                                          size_t limit) const override;
  Roe<std::vector<PostHeadLane>> LanesOf(const std::string& author) const override;
  Roe<bool> HasFork(const std::string& author) const override;
  Roe<void> PutHead(const SignedPostHead& head, const PostHead& parsed) override;
  Roe<std::optional<SignedPostHead>> Head(const std::string& author) const override;
  Roe<std::optional<OwnPostLane>> OwnLane(const std::string& device) const override;
  Roe<void> PutOwnLane(const OwnPostLane& lane) override;

private:
  struct KeptHead {
    int64_t issued_at = 0;
    SignedPostHead head;
  };
  using Place = std::tuple<std::string, PostLane, uint64_t>;

  mutable std::mutex mutex_;
  /** Per place, its entries by hash. */
  std::map<Place, std::map<PostHash, StoredPostEntry>> entries_;
  std::map<std::string, KeptHead> heads_;
  std::map<std::string, OwnPostLane> own_lanes_;
};

} // namespace pbr
