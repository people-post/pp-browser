#pragma once

#include "lib/posts/PostTypes.h"

#include "common/Error.h"
#include "common/Module.h"

#include <mutex>
#include <optional>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

struct sqlite3;

namespace pbr {

/** Why an entry's bytes are not kept (S008, S016): the lane continues past a stub. */
enum class PostStub : uint8_t {
  None = 0,
  Deleted = 1,  // the author deleted it
  Unkept = 2,   // an unknown kind, or past this reader's limits
};

/** One entry as kept: its place and hash always; its signed bytes unless it is a stub. */
struct StoredPostEntry {
  std::string author;
  PostLane lane{};
  uint64_t seq = 0;
  PostHash entry_hash{};
  uint8_t kind = 0;
  int64_t claimed_at = 0;
  uint64_t size = 0;  // signed bytes, kept or not
  std::optional<ByteVector> signed_bytes;
  PostStub stub = PostStub::None;
};

/** This device's own lane (S003): device state, bound to the device that writes it. */
struct OwnPostLane {
  std::string device;  // the device's PeerId: another device never writes this lane
  PostLane lane{};
  uint64_t next_seq = 1;
  PostHash last_hash{};
  int64_t last_claimed_at = 0;
};

/**
 * Posts kept in profile.db (public material, plaintext like peer cards): every author's entries
 * (`post_entries`, forks side by side), each author's newest head statement (`post_heads`), and this
 * device's own lane (`post_own_lanes`). Stores what it is given — callers verify first. Thread-safe.
 */
class PostStore : public Module {
public:
  explicit PostStore(std::string profile_db_path);

  enum class Added { New, Duplicate, Fork };

  /** A verified entry; a fork when another entry holds its (author, lane, seq). A stub of it gains its bytes. */
  Roe<Added> AddEntry(const SignedPostEntry& entry, const PostEntry& parsed, const PostHash& entry_hash);
  /** An entry kept only as its place and hash (an unknown kind, past this reader's limits). */
  Roe<Added> AddStub(const StoredPostEntry& stub);
  /** Drop an entry's bytes, keeping its place and hash (a delete). */
  Roe<void> StubOut(const std::string& author, const PostLane& lane, uint64_t seq, const PostHash& entry_hash,
                    PostStub why);

  /** The entries at one place: more than one is a fork. */
  Roe<std::vector<StoredPostEntry>> At(const std::string& author, const PostLane& lane, uint64_t seq) const;
  /** A lane from `from_seq` up, in order, at most kMaxPostPageEntries (and `limit`). */
  Roe<std::vector<StoredPostEntry>> Range(const std::string& author, const PostLane& lane, uint64_t from_seq,
                                          size_t limit) const;
  /** Each lane's highest seq held (and its hash; the lowest hash when forked there). */
  Roe<std::vector<PostHeadLane>> LanesOf(const std::string& author) const;
  /** Places in the author's lanes held by more than one entry. */
  Roe<bool> HasFork(const std::string& author) const;

  /** Keeps the head with the newest issued_at per author (callers verify it first). */
  Roe<void> PutHead(const SignedPostHead& head, const PostHead& parsed);
  Roe<std::optional<SignedPostHead>> Head(const std::string& author) const;

  Roe<std::optional<OwnPostLane>> OwnLane(const std::string& device) const;
  Roe<void> PutOwnLane(const OwnPostLane& lane);

private:
  class Db;
  Roe<Db> Open() const;

  std::string profile_db_path_;
  mutable std::mutex mutex_;
  mutable bool schema_ready_ = false;
};

} // namespace pbr
