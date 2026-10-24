#pragma once

#include "lib/posts/PostEntry.h"
#include "lib/posts/PostStore.h"

#include "common/Error.h"

#include <functional>
#include <string>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * Writes this device's lane (S003): numbers, chains, signs and keeps each new entry, and issues head
 * statements over every lane of the account it knows. A lane is bound to its device; the writer starts a
 * new lane rather than reuse a sequence number — when this device has none yet, or when the store holds
 * entries in its lane past what it wrote (another copy of the device wrote there, e.g. after a restore).
 */
class PostAuthor {
public:
  /** `device`: this device's PeerId. `random_lane`: fills a new lane id (tests pass a fixed one). */
  PostAuthor(PostStore& store, std::string account_id, std::string device, PostSigner signer,
             std::function<PostLane()> random_lane = {});

  /** A new entry of `kind` with `body` (checked against the protocol limits), at `now_ms` or later. */
  Roe<SignedPostEntry> Append(PostKind kind, const ByteVector& body, int64_t now_ms);
  /** A head statement over the account's lanes held here. */
  Roe<SignedPostHead> IssueHead(int64_t now_ms);

private:
  Roe<OwnPostLane> CurrentLane();

  PostStore& store_;
  std::string account_id_;
  std::string device_;
  PostSigner signer_;
  std::function<PostLane()> random_lane_;
};

} // namespace pbr
