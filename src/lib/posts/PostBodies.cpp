#include "lib/posts/PostBodies.h"

#include "lib/posts/PostWire.h"

#include <utility>

namespace pbr {

namespace {

using post_wire::Reader;
using post_wire::Writer;

void WritePost(Writer& w, const PostBody& body) {
  w.Utf8(body.text);
  w.U64(body.media.size());
  for (const PostMedia& media : body.media) {
    w.Hash(media.content_id);
    w.Utf8(media.mime);
    w.U64(media.size);
  }
  w.Bool(body.reply_to.has_value());
  if (body.reply_to) {
    w.Ref(*body.reply_to);
  }
  w.Bool(body.quote.has_value());
  if (body.quote) {
    w.Ref(*body.quote);
  }
}

bool ReadPost(Reader& r, PostBody& body) {
  body.text = r.Utf8();
  const uint64_t count = r.U64();
  if (r.failed() || count > kMaxPostMedia) {
    return false;
  }
  for (uint64_t i = 0; i < count; ++i) {
    PostMedia media;
    media.content_id = r.Hash();
    media.mime = r.Utf8();
    media.size = r.U64();
    body.media.push_back(std::move(media));
  }
  if (r.Bool()) {
    body.reply_to = r.Ref();
  }
  if (r.Bool()) {
    body.quote = r.Ref();
  }
  return !r.failed();
}

template <typename T, typename Read>
Roe<T> Decode(const ByteVector& bytes, const char* what, Read read) {
  Reader r(bytes);
  T value;
  if (!read(r, value) || !r.AtEnd()) {
    return Error(std::string("post: malformed ") + what);
  }
  return value;
}

Roe<void> CheckRef(const PostRef& ref) {
  if (ref.author.empty() || ref.author.size() > kMaxPostAuthorBytes || !IsWellFormedUtf8(ref.author) ||
      ref.seq == 0) {
    return Error("post: malformed ref");
  }
  return {};
}

Roe<void> CheckOwnRef(const PostRef& ref, const std::string& author) {
  if (auto ok = CheckRef(ref); !ok) {
    return ok;
  }
  if (ref.author != author) {
    return Error("post: names another author's entry");
  }
  return {};
}

Roe<void> CheckPostFields(const PostBody& body) {
  if (body.text.size() > kMaxPostTextBytes || !IsWellFormedUtf8(body.text)) {
    return Error("post: text too long or not UTF-8");
  }
  if (body.media.size() > kMaxPostMedia) {
    return Error("post: too many media");
  }
  for (const PostMedia& media : body.media) {
    if (media.mime.empty() || media.mime.size() > kMaxPostMimeBytes || !IsWellFormedUtf8(media.mime) ||
        media.size == 0 ||
        media.size > kMaxPostMediaObjectBytes) {
      return Error("post: media too large or unnamed");
    }
  }
  if (body.text.empty() && body.media.empty()) {
    return Error("post: empty");
  }
  if (body.reply_to) {
    if (auto ok = CheckRef(*body.reply_to); !ok) {
      return ok;
    }
  }
  if (body.quote) {
    if (auto ok = CheckRef(*body.quote); !ok) {
      return ok;
    }
  }
  return {};
}

} // namespace

ByteVector EncodePostBody(const PostBody& body) {
  Writer w;
  WritePost(w, body);
  return w.Take();
}

ByteVector EncodeEditBody(const EditBody& body) {
  Writer w;
  w.Ref(body.target);
  WritePost(w, body.replacement);
  return w.Take();
}

ByteVector EncodeTargetBody(const TargetBody& body) {
  Writer w;
  w.Ref(body.target);
  return w.Take();
}

ByteVector EncodeReactBody(const ReactBody& body) {
  Writer w;
  w.Ref(body.target);
  w.Utf8(body.reaction);
  return w.Take();
}

ByteVector EncodeProfileBody(const ProfileBody& body) {
  Writer w;
  w.Utf8(body.display_name);
  w.Utf8(body.bio);
  w.Bool(body.avatar.has_value());
  if (body.avatar) {
    w.Hash(*body.avatar);
  }
  w.U64(body.services.size());
  for (const PostService& service : body.services) {
    w.Utf8(service.kind);
    w.Utf8(service.terms);
  }
  return w.Take();
}

ByteVector EncodeLaneCloseBody(const LaneCloseBody& body) {
  Writer w;
  w.Lane(body.lane);
  w.U64(body.last_seq);
  return w.Take();
}

ByteVector EncodeDisavowBody(const DisavowBody& body) {
  Writer w;
  w.U64(body.targets.size());
  for (const PostRef& ref : body.targets) {
    w.Ref(ref);
  }
  return w.Take();
}

Roe<PostBody> DecodePostBody(const ByteVector& bytes) {
  return Decode<PostBody>(bytes, "post", [](Reader& r, PostBody& body) { return ReadPost(r, body); });
}

Roe<EditBody> DecodeEditBody(const ByteVector& bytes) {
  return Decode<EditBody>(bytes, "edit", [](Reader& r, EditBody& body) {
    body.target = r.Ref();
    return ReadPost(r, body.replacement);
  });
}

Roe<TargetBody> DecodeTargetBody(const ByteVector& bytes) {
  return Decode<TargetBody>(bytes, "target", [](Reader& r, TargetBody& body) {
    body.target = r.Ref();
    return !r.failed();
  });
}

Roe<ReactBody> DecodeReactBody(const ByteVector& bytes) {
  return Decode<ReactBody>(bytes, "react", [](Reader& r, ReactBody& body) {
    body.target = r.Ref();
    body.reaction = r.Utf8();
    return !r.failed();
  });
}

Roe<ProfileBody> DecodeProfileBody(const ByteVector& bytes) {
  return Decode<ProfileBody>(bytes, "profile", [](Reader& r, ProfileBody& body) {
    body.display_name = r.Utf8();
    body.bio = r.Utf8();
    if (r.Bool()) {
      body.avatar = r.Hash();
    }
    const uint64_t count = r.U64();
    if (r.failed() || count > kMaxPostServices) {
      return false;
    }
    for (uint64_t i = 0; i < count; ++i) {
      PostService service;
      service.kind = r.Utf8();
      service.terms = r.Utf8();
      body.services.push_back(std::move(service));
    }
    return !r.failed();
  });
}

Roe<LaneCloseBody> DecodeLaneCloseBody(const ByteVector& bytes) {
  return Decode<LaneCloseBody>(bytes, "lane_close", [](Reader& r, LaneCloseBody& body) {
    body.lane = r.Lane();
    body.last_seq = r.U64();
    return !r.failed();
  });
}

Roe<DisavowBody> DecodeDisavowBody(const ByteVector& bytes) {
  return Decode<DisavowBody>(bytes, "disavow", [](Reader& r, DisavowBody& body) {
    const uint64_t count = r.U64();
    // Each ref is at least 65 bytes: the count can be no more than the bytes allow.
    if (r.failed() || count == 0 || count > kMaxPostEntryBytes / 65) {
      return false;
    }
    for (uint64_t i = 0; i < count; ++i) {
      body.targets.push_back(r.Ref());
    }
    return !r.failed();
  });
}

Roe<void> CheckPostBody(const uint8_t kind, const ByteVector& body, const std::string& author) {
  if (!IsKnownPostKind(kind)) {
    return {};
  }
  switch (static_cast<PostKind>(kind)) {
  case PostKind::Post: {
    auto post = DecodePostBody(body);
    return post ? CheckPostFields(post.value()) : Roe<void>(post.error());
  }
  case PostKind::Edit: {
    auto edit = DecodeEditBody(body);
    if (!edit) {
      return edit.error();
    }
    if (auto own = CheckOwnRef(edit.value().target, author); !own) {
      return own;
    }
    return CheckPostFields(edit.value().replacement);
  }
  case PostKind::Delete: {
    auto target = DecodeTargetBody(body);
    return target ? CheckOwnRef(target.value().target, author) : Roe<void>(target.error());
  }
  case PostKind::Repost: {
    auto target = DecodeTargetBody(body);
    return target ? CheckRef(target.value().target) : Roe<void>(target.error());
  }
  case PostKind::React: {
    auto react = DecodeReactBody(body);
    if (!react) {
      return react.error();
    }
    const std::string& reaction = react.value().reaction;
    if (reaction.empty() || reaction.size() > kMaxPostReactionBytes || !IsWellFormedUtf8(reaction)) {
      return Error("post: bad reaction");
    }
    return CheckRef(react.value().target);
  }
  case PostKind::Profile: {
    auto profile = DecodeProfileBody(body);
    if (!profile) {
      return profile.error();
    }
    const ProfileBody& p = profile.value();
    if (p.display_name.size() > kMaxPostDisplayNameBytes || !IsWellFormedUtf8(p.display_name) ||
        p.bio.size() > kMaxPostBioBytes || !IsWellFormedUtf8(p.bio)) {
      return Error("post: profile fields too long or not UTF-8");
    }
    for (const PostService& service : p.services) {
      if (service.kind.empty() || service.kind.size() > kMaxPostServiceKindBytes ||
          service.terms.size() > kMaxPostServiceTermsBytes || !IsWellFormedUtf8(service.kind) ||
          !IsWellFormedUtf8(service.terms)) {
        return Error("post: bad service");
      }
    }
    return {};
  }
  case PostKind::LaneClose: {
    auto close = DecodeLaneCloseBody(body);
    if (!close) {
      return close.error();
    }
    return close.value().last_seq == 0 ? Roe<void>(Error("post: lane_close names no seq")) : Roe<void>{};
  }
  case PostKind::Disavow: {
    auto disavow = DecodeDisavowBody(body);
    if (!disavow) {
      return disavow.error();
    }
    for (const PostRef& ref : disavow.value().targets) {
      if (auto own = CheckOwnRef(ref, author); !own) {
        return own;
      }
    }
    return {};
  }
  }
  return {};
}

bool IsWellFormedUtf8(const std::string& text) {
  const auto* p = reinterpret_cast<const unsigned char*>(text.data());
  const size_t n = text.size();
  size_t i = 0;
  while (i < n) {
    const unsigned char c = p[i];
    if (c < 0x80) {
      ++i;
      continue;
    }
    size_t len = 0;
    uint32_t cp = 0;
    if ((c & 0xE0) == 0xC0) {
      len = 2;
      cp = c & 0x1F;
    } else if ((c & 0xF0) == 0xE0) {
      len = 3;
      cp = c & 0x0F;
    } else if ((c & 0xF8) == 0xF0) {
      len = 4;
      cp = c & 0x07;
    } else {
      return false;
    }
    if (i + len > n) {
      return false;
    }
    for (size_t k = 1; k < len; ++k) {
      if ((p[i + k] & 0xC0) != 0x80) {
        return false;
      }
      cp = (cp << 6) | (p[i + k] & 0x3F);
    }
    const uint32_t min_for_len[] = {0, 0, 0x80, 0x800, 0x10000};
    if (cp < min_for_len[len] || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
      return false;
    }
    i += len;
  }
  return true;
}

} // namespace pbr
