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
  std::string alt;          // for screen readers; empty = none
  uint32_t width = 0;       // pixels; 0 = unknown
  uint32_t height = 0;
  uint32_t duration_ms = 0; // 0 = a still image
  ByteVector placeholder;   // ThumbHash shown while the media loads; empty = none

  bool operator==(const PostMedia&) const = default;
};

enum class PostFacetType : uint8_t {
  Mention = 1,  // value: an account id
  Link = 2,     // value: an https URL
  Hashtag = 3,  // value: the tag without "#"
};

/** A byte span [start, end) of the text and what it means; a reader shows an unknown type as plain text. */
struct PostFacet {
  uint64_t start = 0;
  uint64_t end = 0;
  uint8_t type = 0;
  std::string value;

  bool operator==(const PostFacet&) const = default;
};

/** The thread's top post and the post replied to (the same ref for a reply to a top-level post). */
struct PostReply {
  PostRef root;
  PostRef parent;

  bool operator==(const PostReply&) const = default;
};

/** kind post (and the replacement in an edit): projects/social-posts/DESIGN.md § Post body (S026). */
struct PostBody {
  std::string text;
  std::vector<PostMedia> media;
  std::vector<PostFacet> facets;  // sorted by start, not overlapping
  std::optional<PostReply> reply;
  std::optional<PostRef> quote;
  std::string content_warning;    // empty = none
  std::vector<std::string> labels;     // author self-labels: "sexual", "nudity", "graphic"
  std::vector<std::string> languages;  // BCP 47 tags

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
 * disavow name `author`'s own entries; a post's facets are sorted, apart and on UTF-8 boundaries of its text.
 * An unknown kind passes (it is kept as a stub, not read).
 */
Roe<void> CheckPostBody(uint8_t kind, const ByteVector& body, const std::string& author);

/** Well-formed UTF-8 (no overlongs, surrogates or code points past U+10FFFF). */
bool IsWellFormedUtf8(const std::string& text);

} // namespace pbr
