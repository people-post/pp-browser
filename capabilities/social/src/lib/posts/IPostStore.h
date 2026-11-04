#pragma once

#include "lib/posts/PostTypes.h"

#include "common/Error.h"

#include <optional>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/** Why an entry's bytes are not kept (S008, S016): the lane continues past a stub. */
enum class PostStub : uint8_t {
  None = 0,
  Deleted = 1, // the author deleted it
  Unkept = 2,  // an unknown kind, or past this reader's limits
};

/** One entry as kept: its place and hash always; its signed bytes unless it is a stub. */
struct StoredPostEntry {
  std::string author;
  PostLane lane{};
  uint64_t seq = 0;
  PostHash entry_hash{};
  uint8_t kind = 0;
  int64_t claimed_at = 0;
  uint64_t size = 0; // signed bytes, kept or not
  std::optional<ByteVector> signed_bytes;
  PostStub stub = PostStub::None;
};

/** This device's own lane (S003): device state, bound to the device that writes it. */
struct OwnPostLane {
  std::string device; // the device's PeerId: another device never writes this lane
  PostLane lane{};
  uint64_t next_seq = 1;
  PostHash last_hash{};
  int64_t last_claimed_at = 0;
};

/**
 * Where a device keeps posts (CR024): every author's entries (forks side by side), each author's newest head
 * statement, and this device's own lane. It stores what it is given — callers verify first. The app's storage keeps
 * them (profile.db; public material, plaintext like peer cards); tests use an in-memory one.
 */
class IPostStore {
public:
  virtual ~IPostStore() = default;

  enum class Added { New, Duplicate, Fork };

  /** A verified entry, kept with its signed bytes (AddStub of its row). */
  Roe<Added> AddEntry(const SignedPostEntry& entry, const PostEntry& parsed, const PostHash& entry_hash);
  /**
   * An entry's row: a fork when another entry holds its (author, lane, seq); a duplicate when this one does — then an
   * unkept stub of it gains the bytes, a deleted one stays deleted. A row without bytes is a stub.
   */
  virtual Roe<Added> AddStub(const StoredPostEntry& row) = 0;
  /** Drop an entry's bytes, keeping its place and hash (a delete). */
  virtual Roe<void> StubOut(const std::string& author, const PostLane& lane, uint64_t seq, const PostHash& entry_hash,
                            PostStub why) = 0;

  /** The entries at one place, by hash: more than one is a fork. */
  virtual Roe<std::vector<StoredPostEntry>> At(const std::string& author, const PostLane& lane, uint64_t seq) const = 0;
  /** A lane from `from_seq` up, by seq then hash, at most kMaxPostPageEntries (and `limit`). */
  virtual Roe<std::vector<StoredPostEntry>> Range(const std::string& author, const PostLane& lane, uint64_t from_seq,
                                                  size_t limit) const = 0;
  /** Each lane's highest seq held (and its hash; the lowest hash when forked there), by lane. */
  virtual Roe<std::vector<PostHeadLane>> LanesOf(const std::string& author) const = 0;
  /** Places in the author's lanes held by more than one entry. */
  virtual Roe<bool> HasFork(const std::string& author) const = 0;

  /** Keeps the head with the newest issued_at per author (callers verify it first). */
  virtual Roe<void> PutHead(const SignedPostHead& head, const PostHead& parsed) = 0;
  virtual Roe<std::optional<SignedPostHead>> Head(const std::string& author) const = 0;

  virtual Roe<std::optional<OwnPostLane>> OwnLane(const std::string& device) const = 0;
  virtual Roe<void> PutOwnLane(const OwnPostLane& lane) = 0;
};

} // namespace pbr
