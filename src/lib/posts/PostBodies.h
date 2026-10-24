#pragma once

#include "lib/posts/PostTypes.h"

#include "common/Error.h"

#include <optional>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/** A media object of a post, in the public CAS realm (projects/content-cas). */
struct PostMedia {
  PostHash content_id{};
  std::string mime;
  uint64_t size = 0;

  bool operator==(const PostMedia&) const = default;
};

/** kind post (and the replacement in an edit). */
struct PostBody {
  std::string text;
  std::vector<PostMedia> media;
  std::optional<PostRef> reply_to;
  std::optional<PostRef> quote;

  bool operator==(const PostBody&) const = default;
};

struct EditBody {
  PostRef target;
  PostBody replacement;

  bool operator==(const EditBody&) const = default;
};

/** kinds delete and repost: one target. */
struct TargetBody {
  PostRef target;

  bool operator==(const TargetBody&) const = default;
};

struct ReactBody {
  PostRef target;
  std::string reaction;  // v1 UI: "like"

  bool operator==(const ReactBody&) const = default;
};

/** A service an account offers (S013): its kind (post_relay, witness, anchor) and terms as the provider states them. */
struct PostService {
  std::string kind;
  std::string terms;  // JSON text, signed as given

  bool operator==(const PostService&) const = default;
};

struct ProfileBody {
  std::string display_name;
  std::string bio;
  std::optional<PostHash> avatar;
  std::vector<PostService> services;

  bool operator==(const ProfileBody&) const = default;
};

struct LaneCloseBody {
  PostLane lane{};
  uint64_t last_seq = 0;

  bool operator==(const LaneCloseBody&) const = default;
};

struct DisavowBody {
  std::vector<PostRef> targets;

  bool operator==(const DisavowBody&) const = default;
};

// Canonical encodings (PP binary wire profile, D088). Encoders do not check limits: CheckPostBody does,
// for the author before signing and for every reader.
ByteVector EncodePostBody(const PostBody& body);
ByteVector EncodeEditBody(const EditBody& body);
ByteVector EncodeTargetBody(const TargetBody& body);
ByteVector EncodeReactBody(const ReactBody& body);
ByteVector EncodeProfileBody(const ProfileBody& body);
ByteVector EncodeLaneCloseBody(const LaneCloseBody& body);
ByteVector EncodeDisavowBody(const DisavowBody& body);

Roe<PostBody> DecodePostBody(const ByteVector& bytes);
Roe<EditBody> DecodeEditBody(const ByteVector& bytes);
Roe<TargetBody> DecodeTargetBody(const ByteVector& bytes);
Roe<ReactBody> DecodeReactBody(const ByteVector& bytes);
Roe<ProfileBody> DecodeProfileBody(const ByteVector& bytes);
Roe<LaneCloseBody> DecodeLaneCloseBody(const ByteVector& bytes);
Roe<DisavowBody> DecodeDisavowBody(const ByteVector& bytes);

/**
 * The body decodes for its kind and is within the protocol limits; refs are well formed; edit / delete /
 * disavow name `author`'s own entries. An unknown kind passes (it is kept as a stub, not read).
 */
Roe<void> CheckPostBody(uint8_t kind, const ByteVector& body, const std::string& author);

/** Well-formed UTF-8 (no overlongs, surrogates or code points past U+10FFFF). */
bool IsWellFormedUtf8(const std::string& text);

} // namespace pbr
