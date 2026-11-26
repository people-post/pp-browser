#include "gui/chat/ForwardPreview.h"

#include "foundation/i18n/LocalizationService.h"
#include "lib/messaging/AttachmentCache.h"
#include "lib/voice/VoiceFile.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>

namespace pbr {

namespace {

const char* IconFor(const ForwardItemKind kind) {
  switch (kind) {
    case ForwardItemKind::Picture:
      return "photo";
    case ForwardItemKind::Video:
      return "video";
    case ForwardItemKind::Voice:
      return "mic";
    case ForwardItemKind::File:
      return "file";
    case ForwardItemKind::Card:
      return "card";
    case ForwardItemKind::Text:
    default:
      return "text";
  }
}

std::string MegabytesText(const uint64_t bytes) {
  return std::to_string(std::max<uint64_t>(1, (bytes + 999'999) / 1'000'000));
}

/** The larger side of a JPEG from its SOF segment; 0 when it is not a JPEG or has no readable size. */
int JpegLongSide(const std::vector<uint8_t>& jpeg) {
  if (jpeg.size() < 4 || jpeg[0] != 0xFF || jpeg[1] != 0xD8) {
    return 0;
  }
  size_t i = 2;
  while (i + 4 <= jpeg.size()) {
    if (jpeg[i] != 0xFF) {
      return 0;
    }
    const uint8_t marker = jpeg[i + 1];
    if (marker == 0xFF) {
      ++i; // fill byte
      continue;
    }
    if (marker == 0xD8 || marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) {
      i += 2; // markers without a length
      continue;
    }
    if (marker == 0xD9 || marker == 0xDA) {
      return 0; // end / scan before any size
    }
    const size_t length = (static_cast<size_t>(jpeg[i + 2]) << 8) | jpeg[i + 3];
    const bool sof = marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC;
    if (sof) {
      if (i + 9 > jpeg.size()) {
        return 0;
      }
      const int height = (jpeg[i + 5] << 8) | jpeg[i + 6];
      const int width = (jpeg[i + 7] << 8) | jpeg[i + 8];
      return std::max(width, height);
    }
    if (length < 2) {
      return 0;
    }
    i += 2 + length;
  }
  return 0;
}

} // namespace

ForwardPreviewFiles::ForwardPreviewFiles(std::string profile_dir)
    : dir_((std::filesystem::path(AttachmentViewRoot(profile_dir, kForwardPreviewViewThread))).string()) {}

std::string ForwardPreviewFiles::Put(const std::vector<uint8_t>& jpeg, const size_t index) {
  const int side = JpegLongSide(jpeg);
  if (side <= 0 || side > kForwardPreviewMaxLongSidePx) {
    return {};
  }
  std::error_code ec;
  if (!started_) {
    std::filesystem::remove_all(dir_, ec); // leftovers of a page that was killed
    started_ = true;
  }
  std::filesystem::create_directories(dir_, ec);
  const std::string path = (std::filesystem::path(dir_) / (std::to_string(index) + ".jpg")).string();
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast<const char*>(jpeg.data()), static_cast<std::streamsize>(jpeg.size()));
  return out ? path : std::string();
}

void ForwardPreviewFiles::RemoveAll() {
  std::error_code ec;
  std::filesystem::remove_all(dir_, ec);
  started_ = false;
}

ForwardPreviewContent BuildForwardPreview(const ForwardCheck& check, const std::string& text,
                                          const std::function<std::string(size_t count)>& count_text,
                                          const ForwardPreviewPorts& ports) {
  ForwardPreviewContent content;
  content.text = text;
  if (check.attachments == 0) {
    return content; // text and cards only: the words, as before
  }
  content.text = check.items.size() > 1
                     ? (count_text ? count_text(check.items.size()) : std::to_string(check.items.size()))
                     : std::string();
  size_t index = 0;
  for (const ForwardItemFacts& item : check.items) {
    if (content.items.size() >= kForwardPreviewTiles) {
      break;
    }
    ChoosePreviewItem tile;
    tile.icon = IconFor(item.kind);
    tile.grey = (item.kind != ForwardItemKind::Text && item.kind != ForwardItemKind::Card) && !item.has_local_bytes;
    if (item.kind == ForwardItemKind::File) {
      tile.label = item.filename + (item.byte_length > 0 ? " · " + FormatAttachmentByteSize(item.byte_length) : "");
    } else if (item.kind == ForwardItemKind::Voice) {
      if (const auto ms = ParseVoiceFileName(item.filename)) {
        tile.label = Tr("chat.forward.preview_voice", {{"t", FormatVoiceDuration(*ms)}});
      }
    }
    if (!tile.grey) {
      if (item.kind == ForwardItemKind::Video && ports.poster) {
        tile.thumb_src = ports.poster(item);
      } else if (item.kind == ForwardItemKind::Picture) {
        // The picture the bubble shows, when that row is on the page; else the stored bytes, in memory, and only the
        // few that are shown here (and none above the size cap: it shows the icon, no download, no big read).
        if (ports.bubble_picture) {
          tile.thumb_src = ports.bubble_picture(item.message_id);
        }
        if (tile.thumb_src.empty() && item.byte_length <= kForwardPreviewMaxBytes && ports.plaintext &&
            ports.thumbnail && ports.put_file) {
          if (auto bytes = ports.plaintext(item.content_hash); bytes && bytes->size() <= kForwardPreviewMaxBytes) {
            if (auto jpeg = ports.thumbnail(*bytes, kForwardPreviewThumbPx)) {
              tile.thumb_src = ports.put_file(*jpeg, index);
            }
          }
        }
      }
    }
    ++index;
    content.items.push_back(std::move(tile));
  }
  if (check.without_local_bytes > 0) {
    content.note = Tr("chat.forward.note_without_bytes", {{"n", std::to_string(check.without_local_bytes)}});
  }
  return content;
}

std::string ForwardRefusalText(const ForwardCheck& check) {
  switch (check.refusal) {
    case ForwardRefusal::None:
      return {};
    case ForwardRefusal::AttachmentBusy:
      return Tr("chat.forward.busy");
    case ForwardRefusal::CallActive:
      return Tr("chat.forward.call_active");
    case ForwardRefusal::TooManyUploads:
      return Tr("chat.forward.limit_uploads", {{"n", std::to_string(check.uploads)}});
    case ForwardRefusal::TooLarge:
      return Tr("chat.forward.limit_size", {{"n", MegabytesText(check.upload_bytes)}});
    case ForwardRefusal::SingleTooLarge:
      return Tr("chat.forward.too_big_single", {{"n", MegabytesText(check.largest_bytes)}});
  }
  return {};
}

} // namespace pbr
