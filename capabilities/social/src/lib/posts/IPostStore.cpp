#include "lib/posts/IPostStore.h"

#include "lib/posts/PostEntry.h"

namespace pbr {

Roe<IPostStore::Added> IPostStore::AddEntry(const SignedPostEntry& entry, const PostEntry& parsed,
                                            const PostHash& entry_hash) {
  StoredPostEntry row{.author = parsed.author,
                      .lane = parsed.lane,
                      .seq = parsed.seq,
                      .entry_hash = entry_hash,
                      .kind = parsed.kind,
                      .claimed_at = parsed.claimed_at,
                      .size = entry.canonical.size() + entry.signature.size(),
                      .signed_bytes = SerializeSignedPostEntry(entry),
                      .stub = PostStub::None};
  return AddStub(row);
}

} // namespace pbr
