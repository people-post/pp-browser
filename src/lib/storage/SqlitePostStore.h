#pragma once

#include "lib/posts/IPostStore.h"

#include "common/Module.h"

#include <mutex>
#include <string>
#include "common/PbrCompat.h"

namespace pbr {

/** IPostStore on profile.db (`post_entries`, `post_heads`, `post_own_lanes`). Thread-safe. */
class SqlitePostStore : public Module, public IPostStore {
public:
  explicit SqlitePostStore(std::string profile_db_path);

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
  class Db;
  Roe<Db> Open() const;

  std::string profile_db_path_;
  mutable std::mutex mutex_;
  mutable bool schema_ready_ = false;
};

} // namespace pbr
