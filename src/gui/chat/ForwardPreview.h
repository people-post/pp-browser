#pragma once

#include "feature/presenters/MessageForward.h"
#include "gui/contacts/PeoplePickerChoose.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace pbr {

/**
 * What "Forward to" shows about the chosen messages (DEV/PLAN-message-forward-files-v3.md, design 4): for a selection with
 * attachments, up to three tiles (a picture's thumbnail, a video's poster, or a type icon, grey when this device holds
 * nothing of it) and a line saying how many will not go. Nothing is downloaded; a picture that has no tile on the
 * chat's page is thumbnailed from its stored bytes in memory, for at most the three shown and never above 8 MB.
 */
inline constexpr size_t kForwardPreviewTiles = 3;
inline constexpr uint64_t kForwardPreviewMaxBytes = 8ULL * 1000ULL * 1000ULL;
inline constexpr int kForwardPreviewThumbPx = 160;

/** The reserved name (not a chat) whose `threads/<name>/blobs_view/` holds the thumbnails: the attachment views' wipes cover it. */
inline constexpr const char* kForwardPreviewViewThread = "forward-preview";
/** No file of this kind is larger than this on its long side: a thumbnail, never the picture. */
inline constexpr int kForwardPreviewMaxLongSidePx = 256;

/**
 * The thumbnails made for the "Forward to" page, as files (the UI library loads an `<img>` from a path). They are
 * decrypted content, so they live in the session view area of the profile (`AttachmentViewRoot`): removed when the page
 * closes (RemoveAll, also on lock, sign-out and messaging teardown through ForwardFlow::Stop), by every lock / sign-out
 * wipe of the attachment views (WipeAllAttachmentViewCaches), and once at each start (CleanStaleAttachmentFiles).
 * Names are a counter, never a hash or a message id.
 */
class ForwardPreviewFiles {
public:
  /** `release`: told each file's path when it goes, to let the UI library drop its texture (default: ui::ReleaseTexture). */
  explicit ForwardPreviewFiles(std::string profile_dir, std::function<void(const std::string& path)> release = {});
  /** Writes the JPEG as `<index>.jpg`; returns its path, or empty when it is not a JPEG of at most 256 px a side. */
  std::string Put(const std::vector<uint8_t>& jpeg, size_t index);
  /** Removes the folder and what is in it. */
  void RemoveAll();

private:
  std::string dir_;
  bool started_ = false;
  std::function<void(const std::string& path)> release_;
  std::vector<std::string> made_;
  uint64_t open_serial_ = 0;
};

struct ForwardPreviewPorts {
  /** The file a picture's bubble on screen already shows (empty: the row is not on the page, or has no picture yet). */
  std::function<std::string(const std::string& message_id)> bubble_picture;
  /** The video's `.poster.jpg` in the chat's view cache when it exists (empty: none). */
  std::function<std::string(const ForwardItemFacts& item)> poster;
  /** The stored plaintext of an attachment (decrypts: called only for a picture that is shown and small enough). */
  std::function<Roe<ByteVector>(const std::vector<uint8_t>& content_hash)> plaintext;
  /** A JPEG thumbnail of the bytes, made in memory (MakeImageThumbnailJpeg). */
  std::function<Roe<std::vector<uint8_t>>(const ByteVector& bytes, int max_px)> thumbnail;
  /** Puts the JPEG where an `<img>` can load it (a session folder that goes when the page does); returns its path. */
  std::function<std::string(const std::vector<uint8_t>& jpeg, size_t index)> put_file;
};

struct ForwardPreviewContent {
  /** The strip's words; empty when the tiles say it all (one attachment). */
  std::string text;
  std::vector<ChoosePreviewItem> items;
  /** "{n} aren't on this device and won't be forwarded"; empty when all are here. */
  std::string note;
};

/**
 * `text` is the presenter's strip for the selection (ForwardPreview). With no attachment among the items the result is
 * just that text; with some, tiles for the first three items and the note.
 */
ForwardPreviewContent BuildForwardPreview(const ForwardCheck& check, const std::string& text,
                                          const std::function<std::string(size_t count)>& count_text,
                                          const ForwardPreviewPorts& ports);

/** One picture tile that still lacks its thumbnail (the tile's place in the strip, what to read). */
struct ForwardThumbJob {
  size_t tile = 0;
  uint64_t byte_length = 0;
  std::vector<uint8_t> content_hash;
};

/** What a job made: the JPEG for the tile. A job that could not be made has no result. */
struct ForwardThumbResult {
  size_t tile = 0;
  std::vector<uint8_t> jpeg;
};

/**
 * The picture tiles of `content` (made with `plaintext` / `thumbnail` unset) that may still get a thumbnail from the
 * stored bytes: shown, held on this device, and not above the size cap. UI thread; reads nothing.
 */
std::vector<ForwardThumbJob> ForwardThumbJobsFor(const ForwardCheck& check, const ForwardPreviewContent& content);

/**
 * Reads, decrypts and thumbnails each job (`ports.plaintext`, `ports.thumbnail`, nothing else). Writes no file and
 * touches no UI state: it is the part that runs on a worker thread.
 */
std::vector<ForwardThumbResult> MakeForwardThumbs(const std::vector<ForwardThumbJob>& jobs,
                                                  const ForwardPreviewPorts& ports);

/** The line `Send` is off for, in the user's language; empty for no refusal. */
std::string ForwardRefusalText(const ForwardCheck& check);

} // namespace pbr
