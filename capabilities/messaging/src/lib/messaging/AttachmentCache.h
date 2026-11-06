#pragma once

#include "common/chat/ChatPayloadTypes.h"
#include "foundation/crypto/CryptoTypes.h"

#include "common/Error.h"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/** Session plaintext view root `{profile}/threads/{thread_id}/blobs_view`. */
std::string AttachmentViewRoot(const std::string& profile_dir, const std::string& thread_id);

/** Lowercase hex of attachment plaintext hash. */
std::string AttachmentHashHex(const std::vector<uint8_t>& content_hash);

std::string AttachmentExtensionFromMime(const std::string& mime, const std::string& filename = {});

/**
 * The mime type of a file by its extension (lower-cased). Images (png, jpg, webp, gif), documents (pdf, txt),
 * video (mp4, webm, mov) and audio (mp3, m4a, wav, ogg) are named; everything else is
 * application/octet-stream. Only the type table: nothing is transcoded.
 */
std::string AttachmentMimeFromFilename(const std::string& filename);

/**
 * The extension a file's first bytes say it has (png, jpg, gif, webp, mp4, mov, m4a, webm, ogg, wav, mp3, pdf), or
 * empty when they say nothing. For a pick that carries no name (an Android media-store URI), so it can be typed.
 */
std::string AttachmentExtensionFromContent(const std::vector<uint8_t>& head);

bool IsAttachmentImageMime(const std::string& mime);
bool IsAttachmentVideoMime(const std::string& mime);
bool AttachmentOpenNeedsConfirm(const std::string& mime);

/**
 * Best-effort magic-byte sniff for the mimes AttachmentExtensionFromMime knows about
 * (png/jpeg/gif/webp/mp4/webm/pdf). Returns true (nothing to flag) when the mime has no
 * checkable signature, the file is missing/too short, or the signature matches; false when
 * the declared mime and the file's actual content clearly disagree (sender lied about mime
 * to bypass the open-without-confirm path for images/videos).
 */
bool AttachmentContentMatchesMime(const std::string& path, const std::string& mime);

/**
 * True only when all of: mime is one of the image/video mimes AttachmentExtensionFromMime
 * knows about, path's actual extension equals AttachmentExtensionFromMime(mime) exactly, and
 * AttachmentContentMatchesMime confirms the file's content. Anything else (an unlisted mime
 * such as image/svg+xml or video/quicktime, a mismatched extension, or mismatched content)
 * is not safe to open without confirmation.
 */
bool AttachmentSafeToAutoOpen(const std::string& path, const std::string& mime);

/**
 * The name a file is handed to the system viewer under: the sender's name without any folder part, control or
 * reserved characters, leading dots or more than 100 bytes, ending in the safe extension AttachmentExtensionFromMime
 * gives (appended when the name lacks it); `fallback_stem` when nothing of the name is left. Empty when the type
 * has no safe extension: the stored file is opened as it is (an executable is never given its name back).
 * `content_ext` (a sound file's sniffed container: "mp3", "m4a" ...; empty when unknown): when the name's extension
 * does not say it, it is appended too (song.mp3 that is really MPEG-4 opens as song.mp3.m4a): the system viewers
 * type by extension. The caller sniffs (lib/voice is a peer, not a dependency).
 */
std::string AttachmentOpenFilename(const std::string& filename, const std::string& mime,
                                   const std::string& fallback_stem, const std::string& content_ext = {});
/**
 * The name's own extension `own` already says what `ext` (the type's safe extension) says: the same, another
 * spelling (jpeg / jpg, mpga / mp3, mp4 / m4a for a sound), or a known format of the same kind (audio, image or
 * video: song.mp3 sent as audio/mp4). Then AttachmentOpenFilename keeps the name as it is.
 */
bool AttachmentExtensionMatchesType(const std::string& own, const std::string& ext, const std::string& mime);

/**
 * The view file `view_path` under its AttachmentOpenFilename, as a hard link (else a copy) in
 * `{blobs_view}/{hash}.open/`, so the viewer shows the real name and knows the type (iOS QuickLook decides by the
 * extension). It goes with the view cache and with the attachment's local delete. `view_path` itself when there is
 * no safe name or the link cannot be made.
 */
std::string AttachmentNamedOpenPath(const std::string& profile_dir, const std::string& thread_id,
                                    const std::vector<uint8_t>& content_hash, const std::string& mime,
                                    const std::string& filename, const std::string& view_path,
                                    const std::string& content_ext = {});
/** `{blobs_view}/{hash}.open` (see AttachmentNamedOpenPath). */
std::string AttachmentOpenLinkDir(const std::string& profile_dir, const std::string& thread_id,
                                  const std::vector<uint8_t>& content_hash);

/**
 * True when private attachment plaintext may be materialized for inline UI
 * (`blobs_view` / image src). A video above `max_receive_bytes` (AttachmentLimits) returns false: it is
 * never downloaded, so nothing is materialized. Unknown size (`byte_length == 0`) stays permissive.
 */
bool AttachmentAllowsInlinePrivateView(const std::string& mime, uint64_t byte_length, uint64_t max_receive_bytes);

std::string FormatAttachmentByteSize(uint64_t byte_length);

/**
 * Absolute path to a plaintext file for display/open under `blobs_view/` only.
 * Empty if the view is not materialized.
 */
std::string AttachmentLocalPath(const std::string& profile_dir, const std::string& thread_id,
                                const std::vector<uint8_t>& content_hash, const std::string& mime,
                                const std::string& filename = {});

/**
 * Ensure a plaintext file under `blobs_view/`, from `load_plaintext` (the private content store)
 * when it is not there yet.
 */
/**
 * Write the session view file from bytes the caller already holds (a file just sent): no read of the stored copy
 * back, so a large send does not hold the file a second and third time. Returns the existing view when there is one.
 */
Roe<std::string> StoreAttachmentViewBytes(const std::string& profile_dir, const std::string& thread_id,
                                          const std::vector<uint8_t>& content_hash, const std::string& mime,
                                          const std::string& filename, const ByteVector& plaintext);

Roe<std::string> EnsureAttachmentViewPath(const std::string& profile_dir, const std::string& thread_id,
                                          const std::vector<uint8_t>& content_hash, const std::string& mime,
                                          const std::string& filename,
                                          const std::function<Roe<ByteVector>()>& load_plaintext);

/** `{blobs_view}/{hash}.poster.jpg` — session plaintext poster (wiped with view cache). */
std::string AttachmentPosterPath(const std::string& profile_dir, const std::string& thread_id,
                                 const std::vector<uint8_t>& content_hash);

bool AttachmentPosterExists(const std::string& profile_dir, const std::string& thread_id,
                            const std::vector<uint8_t>& content_hash);

/**
 * Ensure a JPEG poster under blobs_view for a video attachment (R012).
 * Best-effort extract; soft placeholder when over inline size or on decode failure.
 * `known_byte_length` skips full decrypt/materialize when the video exceeds `max_receive_bytes`.
 */
Roe<std::string> EnsureAttachmentPoster(const std::string& profile_dir, const std::string& thread_id,
                                        const std::vector<uint8_t>& content_hash, const std::string& mime,
                                        const std::string& filename,
                                        const std::function<Roe<ByteVector>()>& load_plaintext,
                                        uint64_t known_byte_length, uint64_t max_receive_bytes);

/**
 * Ensure a low-res JPEG thumbnail of an image attachment, stored at the poster path (so it is wiped with the view
 * cache). Made by the platform's `image_thumbnail_jpeg` from `image_bytes` (the plaintext, in memory: only the
 * thumbnail is written). Fails (nothing stored) without the hook or when the picture cannot be thumbnailed.
 */
Roe<std::string> EnsureAttachmentImageThumbnail(const std::string& profile_dir, const std::string& thread_id,
                                                const std::vector<uint8_t>& content_hash, const std::string& mime,
                                                const ByteVector& image_bytes);

/**
 * A sound file's cover for its card: a small JPEG made by the platform's `image_thumbnail_jpeg` from `picture` (the
 * file's embedded picture, in memory), stored at the poster path, so it goes with the view cache and the
 * attachment's local delete like a picture's thumbnail. Local only: never sent.
 */
Roe<std::string> EnsureAttachmentAudioCover(const std::string& profile_dir, const std::string& thread_id,
                                            const std::vector<uint8_t>& content_hash, const ByteVector& picture);

/** Remove view / pending-cipher caches for a thread (and any leftover pre-cutover `blobs/` dir). */
Roe<void> WipeThreadAttachmentBlobs(const std::string& profile_dir, const std::string& thread_id);

/** Wipe all `blobs_view/` trees (ClearDek / vault lock). */
Roe<void> WipeAllAttachmentViewCaches(const std::string& profile_dir);

/** Pending peer-push ciphertext before envelope key arrives (a6). */
std::string AttachmentPendingCiphertextRoot(const std::string& profile_dir, const std::string& thread_id);

/** Bytes of all pending (pushed, not yet opened) attachment ciphertext, over every thread. */
uint64_t PendingAttachmentCiphertextBytes(const std::string& profile_dir);
/**
 * Store pushed ciphertext for `content_hash`. `referenced_hex` holds the (lower-case hex) hashes
 * the thread's attachment messages name: those blobs are real attachments — possibly left
 * unopened because auto-download deferred them — and are never pruned or counted. Only orphan
 * pushes (no matching message yet) are bounded: they expire after 24 h and a thread holds at
 * most 64 of them.
 */
Roe<void> SavePendingAttachmentCiphertext(const std::string& profile_dir, const std::string& thread_id,
                                          const std::vector<uint8_t>& content_hash,
                                          const std::vector<uint8_t>& ciphertext,
                                          const std::unordered_set<std::string>& referenced_hex = {});
bool AttachmentPendingCiphertextExists(const std::string& profile_dir, const std::string& thread_id,
                                       const std::vector<uint8_t>& content_hash);
Roe<ByteVector> LoadPendingAttachmentCiphertext(const std::string& profile_dir, const std::string& thread_id,
                                                const std::vector<uint8_t>& content_hash);
void RemovePendingAttachmentCiphertext(const std::string& profile_dir, const std::string& thread_id,
                                       const std::vector<uint8_t>& content_hash);

/** Wipe downloaded attachment bytes for every thread and clear private CAS. */
Roe<void> WipeAllAttachmentCaches(const std::string& profile_dir);

} // namespace pbr
