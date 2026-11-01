#pragma once

#include "foundation/crypto/CryptoTypes.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * Public posts (projects/social-posts/DESIGN.md): each device writes a lane — a hash chain of signed
 * entries under the account. These are the shapes; PostEntry.h / PostHead.h encode and check them.
 */

inline constexpr size_t kPostLaneBytes = 16;
inline constexpr size_t kPostHashBytes = 32;
using PostLane = std::array<uint8_t, kPostLaneBytes>;
using PostHash = std::array<uint8_t, kPostHashBytes>;

// Protocol limits (S014). Raised later, never lowered for this entry version (S016).
inline constexpr size_t kMaxPostEntryBytes = 8 * 1024;  // canonical bytes, signature apart
inline constexpr size_t kMaxPostTextBytes = 4 * 1024;
inline constexpr size_t kMaxPostMedia = 4;
inline constexpr uint64_t kMaxPostMediaObjectBytes = 16ull * 1024 * 1024;
inline constexpr size_t kMaxPostAltBytes = 1024;
inline constexpr size_t kMaxPostPlaceholderBytes = 32;
inline constexpr size_t kMaxPostFacets = 32;
inline constexpr size_t kMaxPostMentions = 16;
inline constexpr size_t kMaxPostLinkUrlBytes = 512;
inline constexpr size_t kMaxPostHashtagBytes = 64;
inline constexpr size_t kMaxPostContentWarningBytes = 256;
inline constexpr size_t kMaxPostLabels = 4;
inline constexpr size_t kMaxPostLabelBytes = 32;
inline constexpr size_t kMaxPostLanguages = 3;
inline constexpr size_t kMaxPostLanguageBytes = 35;
inline constexpr size_t kMaxPostExtensions = 16;
inline constexpr size_t kMaxPostHeadLanes = 64;
inline constexpr size_t kMaxPostHeadTargets = 8;
inline constexpr size_t kMaxPostTargetUrlBytes = 256;
inline constexpr size_t kMaxPostPageEntries = 256;
inline constexpr int64_t kMaxPostClockSkewMs = 5 * 60 * 1000;
// Bounds on the smaller fields (all within the entry limit anyway).
inline constexpr size_t kMaxPostAuthorBytes = 128;
inline constexpr size_t kMaxPostMimeBytes = 128;
inline constexpr size_t kMaxPostReactionBytes = 32;
inline constexpr size_t kMaxPostDisplayNameBytes = 128;
inline constexpr size_t kMaxPostBioBytes = 1024;
inline constexpr size_t kMaxPostServices = 8;
inline constexpr size_t kMaxPostServiceKindBytes = 64;
inline constexpr size_t kMaxPostServiceTermsBytes = 1024;

enum class PostKind : uint8_t {
  Post = 1,
  Edit = 2,
  Delete = 3,
  React = 4,
  Repost = 5,
  Profile = 6,
  LaneClose = 7,
  Disavow = 8,
};

/** A kind this version understands (others are kept as stubs and skipped, S016). */
bool IsKnownPostKind(uint8_t kind);

/** One exact entry: the hash pins the content, so a ref survives a later edit of what it named. */
struct PostRef {
  std::string author;
  PostLane lane{};
  uint64_t seq = 0;
  PostHash entry_hash{};

  bool operator==(const PostRef&) const = default;
};

/**
 * Additive data at the end of every entry (S027): signed and hashed with the rest, skipped by readers that do
 * not know the tag. A reader that ignores an extension must still show the entry correctly.
 */
struct PostExtension {
  uint16_t tag = 0;
  ByteVector value;

  bool operator==(const PostExtension&) const = default;
};

/** An entry as it sits in a lane; `body` is the kind's encoding (PostBodies.h). */
struct PostEntry {
  std::string author;
  PostLane lane{};
  uint64_t seq = 0;
  PostHash prev{};
  int64_t claimed_at = 0;
  uint8_t kind = 0;
  ByteVector body;
  std::vector<PostExtension> extensions;  // tags strictly increasing

  bool operator==(const PostEntry&) const = default;
};

/** Canonical bytes plus the account's ML-DSA-65 signature over them; the hash is of the bytes alone (S004). */
struct SignedPostEntry {
  ByteVector canonical;
  ByteVector signature;
};

/** What a head statement says of one lane. */
struct PostHeadLane {
  PostLane lane{};
  uint64_t seq = 0;
  PostHash entry_hash{};

  bool operator==(const PostHeadLane&) const = default;
};

/** An author's lanes as one device sees them, signed so that a carrier cannot hide a lane. */
struct PostHead {
  std::string author;
  int64_t issued_at = 0;
  std::vector<PostHeadLane> lanes;  // sorted by lane, distinct
  std::vector<std::string> targets; // publish targets: https base URLs (S017)

  bool operator==(const PostHead&) const = default;
};

struct SignedPostHead {
  ByteVector canonical;
  ByteVector signature;
};

} // namespace pbr
