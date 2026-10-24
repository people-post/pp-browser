#include "lib/posts/PostEntry.h"

#include "lib/posts/PostBodies.h"
#include "lib/posts/PostWire.h"

#include "foundation/crypto/CryptoUtil.h"
#include "foundation/crypto/MlDsa.h"

#include <algorithm>
#include <utility>

namespace pbr {

namespace {

const std::string kEntryDomain("pp-browser:post-entry-v1\0", 25);
const std::string kHeadDomain("pp-browser:post-head-v1\0", 24);
constexpr uint8_t kEntryVersion = 1;
constexpr uint8_t kHeadVersion = 1;

PostHash HashOf(const ByteVector& bytes) {
  const ByteVector digest = Blake2b256(bytes);
  PostHash hash{};
  std::copy(digest.begin(), digest.end(), hash.begin());
  return hash;
}

bool IsZero(const PostHash& hash) {
  return std::all_of(hash.begin(), hash.end(), [](uint8_t b) { return b == 0; });
}

/** `key` is the account's key: it hashes to `account_id` and signed `canonical`. */
Roe<void> CheckAccountSignature(const std::string& account_id, const ByteVector& key, const ByteVector& canonical,
                                const ByteVector& signature) {
  auto key_account = AccountIdFromMlDsaPublicKey(key);
  if (!key_account || key_account.value() != account_id) {
    return Error("post: key is not the author's");
  }
  if (signature.size() != kMlDsa65SignatureBytes) {
    return Error("post: bad signature size");
  }
  auto verified = MlDsa::Verify(key, canonical, signature);
  if (!verified || !verified.value()) {
    return Error("post: signature does not verify");
  }
  return {};
}

} // namespace

bool IsKnownPostKind(const uint8_t kind) {
  return kind >= static_cast<uint8_t>(PostKind::Post) && kind <= static_cast<uint8_t>(PostKind::Disavow);
}

ByteVector PostEntryCanonicalBytes(const PostEntry& entry) {
  post_wire::Writer w;
  w.Raw(kEntryDomain);
  w.U8(kEntryVersion);
  w.Utf8(entry.author);
  w.Lane(entry.lane);
  w.U64(entry.seq);
  w.Hash(entry.prev);
  w.I64(entry.claimed_at);
  w.U8(entry.kind);
  w.Bytes(entry.body);
  return w.Take();
}

Roe<PostEntry> ParsePostEntry(const ByteVector& canonical) {
  if (canonical.size() > kMaxPostEntryBytes) {
    return Error("post: entry over the size limit");
  }
  post_wire::Reader r(canonical);
  if (!r.Expect(kEntryDomain) || r.U8() != kEntryVersion) {
    return Error("post: not a v1 entry");
  }
  PostEntry entry;
  entry.author = r.Utf8();
  entry.lane = r.Lane();
  entry.seq = r.U64();
  entry.prev = r.Hash();
  entry.claimed_at = r.I64();
  entry.kind = r.U8();
  entry.body = r.Bytes();
  if (!r.AtEnd()) {
    return Error("post: malformed entry");
  }
  return entry;
}

PostHash PostEntryHash(const ByteVector& canonical) {
  return HashOf(canonical);
}

Roe<SignedPostEntry> SignPostEntry(const PostEntry& entry, const PostSigner& signer) {
  SignedPostEntry signed_entry;
  signed_entry.canonical = PostEntryCanonicalBytes(entry);
  if (signed_entry.canonical.size() > kMaxPostEntryBytes) {
    return Error("post: entry over the size limit");
  }
  auto signature = signer(signed_entry.canonical);
  if (!signature) {
    return signature.error();
  }
  signed_entry.signature = std::move(signature.value());
  return signed_entry;
}

ByteVector SerializeSignedPostEntry(const SignedPostEntry& entry) {
  ByteVector bytes = entry.canonical;
  bytes.insert(bytes.end(), entry.signature.begin(), entry.signature.end());
  return bytes;
}

Roe<SignedPostEntry> DeserializeSignedPostEntry(const ByteVector& bytes) {
  if (bytes.size() <= kMlDsa65SignatureBytes || bytes.size() > kMaxPostEntryBytes + kMlDsa65SignatureBytes) {
    return Error("post: bad signed entry size");
  }
  const auto split = bytes.end() - static_cast<std::ptrdiff_t>(kMlDsa65SignatureBytes);
  return SignedPostEntry{ByteVector(bytes.begin(), split), ByteVector(split, bytes.end())};
}

Roe<PostEntry> VerifyPostEntry(const SignedPostEntry& signed_entry, const ByteVector& author_signing_key,
                               const int64_t now_ms) {
  auto parsed = ParsePostEntry(signed_entry.canonical);
  if (!parsed) {
    return parsed.error();
  }
  const PostEntry& entry = parsed.value();
  if (entry.author.empty() || entry.author.size() > kMaxPostAuthorBytes) {
    return Error("post: bad author");
  }
  if (auto signed_ok =
          CheckAccountSignature(entry.author, author_signing_key, signed_entry.canonical, signed_entry.signature);
      !signed_ok) {
    return signed_ok.error();
  }
  if (entry.seq == 0 || (entry.seq == 1) != IsZero(entry.prev)) {
    return Error("post: seq and prev disagree");
  }
  if (entry.kind == 0) {
    return Error("post: no kind");
  }
  if (entry.claimed_at > now_ms + kMaxPostClockSkewMs) {
    return Error("post: claimed time is in the future");
  }
  if (auto body = CheckPostBody(entry.kind, entry.body, entry.author); !body) {
    return body.error();
  }
  return parsed;
}

Roe<void> CheckPostChain(const PostEntry& prev, const PostHash& prev_hash, const PostEntry& next) {
  if (prev.author != next.author || prev.lane != next.lane) {
    return Error("post: not the same lane");
  }
  if (next.seq != prev.seq + 1 || next.prev != prev_hash) {
    return Error("post: does not follow");
  }
  if (next.claimed_at < prev.claimed_at) {
    return Error("post: claimed time goes back");
  }
  return {};
}

bool IsPostFork(const PostEntry& a, const PostHash& a_hash, const PostEntry& b, const PostHash& b_hash) {
  return a.author == b.author && a.lane == b.lane && a.seq == b.seq && a_hash != b_hash;
}

PostRef PostRefOf(const PostEntry& entry, const PostHash& entry_hash) {
  return PostRef{.author = entry.author, .lane = entry.lane, .seq = entry.seq, .entry_hash = entry_hash};
}

ByteVector PostHeadCanonicalBytes(const PostHead& head) {
  std::vector<PostHeadLane> lanes = head.lanes;
  std::sort(lanes.begin(), lanes.end(), [](const PostHeadLane& a, const PostHeadLane& b) { return a.lane < b.lane; });
  post_wire::Writer w;
  w.Raw(kHeadDomain);
  w.U8(kHeadVersion);
  w.Utf8(head.author);
  w.I64(head.issued_at);
  w.U64(lanes.size());
  for (const PostHeadLane& lane : lanes) {
    w.Lane(lane.lane);
    w.U64(lane.seq);
    w.Hash(lane.entry_hash);
  }
  w.U64(head.targets.size());
  for (const std::string& target : head.targets) {
    w.Utf8(target);
  }
  return w.Take();
}

Roe<PostHead> ParsePostHead(const ByteVector& canonical) {
  if (canonical.size() > kMaxPostEntryBytes) {  // 64 lanes are 3.5 KiB
    return Error("post: head over the size limit");
  }
  post_wire::Reader r(canonical);
  if (!r.Expect(kHeadDomain) || r.U8() != kHeadVersion) {
    return Error("post: not a v1 head");
  }
  PostHead head;
  head.author = r.Utf8();
  head.issued_at = r.I64();
  const uint64_t count = r.U64();
  if (r.failed() || count > kMaxPostHeadLanes) {
    return Error("post: head has too many lanes");
  }
  for (uint64_t i = 0; i < count; ++i) {
    PostHeadLane lane;
    lane.lane = r.Lane();
    lane.seq = r.U64();
    lane.entry_hash = r.Hash();
    if (lane.seq == 0 || (!head.lanes.empty() && !(head.lanes.back().lane < lane.lane))) {
      return Error("post: head lanes unsorted, repeated or empty");
    }
    head.lanes.push_back(lane);
  }
  const uint64_t targets = r.U64();
  if (r.failed() || targets > kMaxPostHeadTargets) {
    return Error("post: head has too many targets");
  }
  for (uint64_t i = 0; i < targets; ++i) {
    std::string target = r.Utf8();
    if (target.size() > kMaxPostTargetUrlBytes || target.rfind("https://", 0) != 0) {
      return Error("post: a head target is not an https URL");
    }
    head.targets.push_back(std::move(target));
  }
  if (!r.AtEnd() || head.author.empty() || head.author.size() > kMaxPostAuthorBytes) {
    return Error("post: malformed head");
  }
  return head;
}

PostHash PostHeadHash(const ByteVector& canonical) {
  return HashOf(canonical);
}

Roe<SignedPostHead> SignPostHead(const PostHead& head, const PostSigner& signer) {
  if (head.lanes.size() > kMaxPostHeadLanes) {
    return Error("post: head has too many lanes");
  }
  if (head.targets.size() > kMaxPostHeadTargets ||
      std::any_of(head.targets.begin(), head.targets.end(), [](const std::string& target) {
        return target.size() > kMaxPostTargetUrlBytes || target.rfind("https://", 0) != 0;
      })) {
    return Error("post: head targets must be at most 8 https URLs");
  }
  SignedPostHead signed_head;
  signed_head.canonical = PostHeadCanonicalBytes(head);
  auto signature = signer(signed_head.canonical);
  if (!signature) {
    return signature.error();
  }
  signed_head.signature = std::move(signature.value());
  return signed_head;
}

Roe<PostHead> VerifyPostHead(const SignedPostHead& signed_head, const ByteVector& author_signing_key,
                             const int64_t now_ms) {
  auto head = ParsePostHead(signed_head.canonical);
  if (!head) {
    return head.error();
  }
  if (auto signed_ok =
          CheckAccountSignature(head.value().author, author_signing_key, signed_head.canonical, signed_head.signature);
      !signed_ok) {
    return signed_ok.error();
  }
  if (head.value().issued_at > now_ms + kMaxPostClockSkewMs) {
    return Error("post: head issued in the future");
  }
  return head;
}

} // namespace pbr
