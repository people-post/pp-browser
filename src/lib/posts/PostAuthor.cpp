#include "lib/posts/PostAuthor.h"

#include "lib/posts/PostBodies.h"

#include "foundation/crypto/CryptoUtil.h"

#include <sodium.h>

#include <algorithm>
#include <utility>

namespace pbr {

namespace {

PostLane RandomLane() {
  EnsureSodiumInit();
  PostLane lane{};
  randombytes_buf(lane.data(), lane.size());
  return lane;
}

} // namespace

PostAuthor::PostAuthor(PostStore& store, std::string account_id, std::string device, PostSigner signer,
                       std::function<PostLane()> random_lane)
    : store_(store), account_id_(std::move(account_id)), device_(std::move(device)), signer_(std::move(signer)),
      random_lane_(random_lane ? std::move(random_lane) : std::function<PostLane()>(RandomLane)) {}

Roe<OwnPostLane> PostAuthor::CurrentLane() {
  auto own = store_.OwnLane(device_);
  if (!own) {
    return own.error();
  }
  if (own.value()) {
    // Entries in the lane past what this device wrote: another copy of it wrote there. Never reuse a seq.
    auto beyond = store_.At(account_id_, own.value()->lane, own.value()->next_seq);
    if (!beyond) {
      return beyond.error();
    }
    if (beyond.value().empty()) {
      return *own.value();
    }
  }
  return OwnPostLane{.device = device_, .lane = random_lane_()};
}

Roe<SignedPostEntry> PostAuthor::Append(const PostKind kind, const ByteVector& body, const int64_t now_ms) {
  const auto kind_byte = static_cast<uint8_t>(kind);
  if (auto checked = CheckPostBody(kind_byte, body, account_id_); !checked) {
    return checked.error();
  }
  auto lane = CurrentLane();
  if (!lane) {
    return lane.error();
  }
  OwnPostLane& own = lane.value();
  PostEntry entry{.author = account_id_,
                  .lane = own.lane,
                  .seq = own.next_seq,
                  .prev = own.next_seq == 1 ? PostHash{} : own.last_hash,
                  .claimed_at = std::max(now_ms, own.last_claimed_at),  // never back in time within a lane
                  .kind = kind_byte,
                  .body = body};
  auto signed_entry = SignPostEntry(entry, signer_);
  if (!signed_entry) {
    return signed_entry.error();
  }
  const PostHash hash = PostEntryHash(signed_entry.value().canonical);
  if (auto added = store_.AddEntry(signed_entry.value(), entry, hash); !added) {
    return added.error();
  }
  own.next_seq = entry.seq + 1;
  own.last_hash = hash;
  own.last_claimed_at = entry.claimed_at;
  if (auto saved = store_.PutOwnLane(own); !saved) {
    return saved.error();
  }
  return signed_entry;
}

Roe<SignedPostHead> PostAuthor::IssueHead(const int64_t now_ms) {
  auto lanes = store_.LanesOf(account_id_);
  if (!lanes) {
    return lanes.error();
  }
  PostHead head{.author = account_id_, .issued_at = now_ms, .lanes = std::move(lanes.value())};
  if (head.lanes.size() > kMaxPostHeadLanes) {
    // A head names every lane or it lets a carrier hide one; 64 devices' lanes is the protocol's bound.
    return Error("post: more lanes than a head can name");
  }
  auto signed_head = SignPostHead(head, signer_);
  if (!signed_head) {
    return signed_head.error();
  }
  if (auto kept = store_.PutHead(signed_head.value(), head); !kept) {
    return kept.error();
  }
  return signed_head;
}

} // namespace pbr
