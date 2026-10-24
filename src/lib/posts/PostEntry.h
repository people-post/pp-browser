#pragma once

#include "lib/posts/PostTypes.h"

#include "common/Error.h"

#include <functional>
#include <string>
#include "common/PbrCompat.h"

namespace pbr {

/** Signs with the account's ML-DSA-65 key (the identity's SignBytes). */
using PostSigner = std::function<Roe<ByteVector>(const ByteVector& message)>;

/**
 * Canonical bytes (domain "pp-browser:post-entry-v1" NUL, version 1, author, lane, seq, prev,
 * claimed_at, kind, body) — what is hashed and signed (projects/social-posts/DESIGN.md § Entry).
 */
ByteVector PostEntryCanonicalBytes(const PostEntry& entry);
/** The entry the bytes encode (no signature or validity check); refuses anything past the entry limit. */
Roe<PostEntry> ParsePostEntry(const ByteVector& canonical);
/** BLAKE2b-256 of the canonical bytes; the signature is not part of an entry's identity (S004). */
PostHash PostEntryHash(const ByteVector& canonical);

Roe<SignedPostEntry> SignPostEntry(const PostEntry& entry, const PostSigner& signer);
/** As stored and served: canonical bytes ‖ signature. */
ByteVector SerializeSignedPostEntry(const SignedPostEntry& entry);
Roe<SignedPostEntry> DeserializeSignedPostEntry(const ByteVector& bytes);

/**
 * An entry valid on its own: it parses within limits, `author_signing_key` is the author's account key
 * (hashes to the account id) and signed it, seq and prev agree, claimed_at is at most 5 minutes ahead of
 * `now_ms`, and a known kind's body checks out. Returns the entry.
 */
Roe<PostEntry> VerifyPostEntry(const SignedPostEntry& signed_entry, const ByteVector& author_signing_key,
                               int64_t now_ms);

/** `next` follows `prev` (whose hash is `prev_hash`) in one lane: seq + 1, prev hash, claimed time not earlier. */
Roe<void> CheckPostChain(const PostEntry& prev, const PostHash& prev_hash, const PostEntry& next);

/** Two valid entries at one (author, lane, seq) with different hashes: the author signed two histories. */
bool IsPostFork(const PostEntry& a, const PostHash& a_hash, const PostEntry& b, const PostHash& b_hash);

/** The ref naming this entry. */
PostRef PostRefOf(const PostEntry& entry, const PostHash& entry_hash);

/** Canonical bytes of a head statement (domain "pp-browser:post-head-v1"); lanes sorted and distinct, then targets. */
ByteVector PostHeadCanonicalBytes(const PostHead& head);
Roe<PostHead> ParsePostHead(const ByteVector& canonical);
PostHash PostHeadHash(const ByteVector& canonical);
Roe<SignedPostHead> SignPostHead(const PostHead& head, const PostSigner& signer);
/** A head statement signed by its author's account key, at most 64 lanes, not from the future. */
Roe<PostHead> VerifyPostHead(const SignedPostHead& signed_head, const ByteVector& author_signing_key, int64_t now_ms);

} // namespace pbr
