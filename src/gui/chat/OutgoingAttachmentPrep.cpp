#include "gui/chat/OutgoingAttachmentPrep.h"

#include "foundation/i18n/LocalizationService.h"
#include "foundation/platform/AiImagePrep.h"
#include "foundation/platform/ImageMetadata.h"
#include "lib/messaging/AttachmentCache.h"

#include <filesystem>
#include <fstream>
#include <iterator>

namespace pbr {

namespace {

Error PrepError(const OutgoingAttachmentError kind, const std::string& detail, std::string user = {}) {
  Error err(static_cast<int32_t>(kind), detail);
  if (!user.empty()) {
    err.WithUser(user);
  }
  return err;
}

Error TooLarge(const uint64_t max_bytes) {
  return PrepError(OutgoingAttachmentError::TooLarge, "Attachment over the size limit",
                   Tr("chat.attachment.too_large", {{"limit", FormatAttachmentLimit(max_bytes)}}));
}

Error Unprocessable() {
  return PrepError(OutgoingAttachmentError::ImageUnprocessable, "Picture could not be read or cleaned",
                   Tr("chat.attachment.image_unprocessable"));
}

std::string StemOf(const std::string& filename) { return std::filesystem::path(filename).stem().string(); }

/** Read a whole file, refusing one over `max_bytes` before it is opened. */
Roe<std::vector<uint8_t>> ReadAll(const std::string& path, const uint64_t max_bytes) {
  std::error_code ec;
  const auto size = std::filesystem::file_size(path, ec);
  if (!ec && size > max_bytes) {
    return TooLarge(max_bytes);
  }
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return PrepError(OutgoingAttachmentError::Unreadable, "Could not read attachment file",
                     Tr("chat.attachment.read_failed"));
  }
  // One allocation of the file's size (an iterator copy doubles the vector while it grows).
  std::vector<uint8_t> bytes(ec ? 0 : static_cast<size_t>(size));
  if (!ec) {
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    bytes.resize(static_cast<size_t>(input.gcount()));
  } else {
    bytes.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
  }
  if (bytes.empty()) {
    return PrepError(OutgoingAttachmentError::Empty, "Attachment file is empty", Tr("chat.attachment.read_failed"));
  }
  if (bytes.size() > max_bytes) {
    return TooLarge(max_bytes);
  }
  return bytes;
}

} // namespace

Roe<OutgoingAttachment> PrepareOutgoingAttachment(const std::string& path, const AttachmentLimits& limits,
                                                  const OutgoingPickSource source) {
  OutgoingAttachment out;
  out.filename = std::filesystem::path(path).filename().string();
  if (out.filename.empty()) {
    out.filename = "attachment";
  }
  out.mime = AttachmentMimeFromFilename(out.filename);

  // The content decides what it is; the name only says what the sender called it.
  const ImageKind kind = ImageKindOfFile(path);
  const bool shrinkable = kind == ImageKind::Jpeg || kind == ImageKind::Png || kind == ImageKind::Webp;
  const bool named_as_picture = out.mime == "image/png" || out.mime == "image/jpeg" || out.mime == "image/webp";

  if (shrinkable && source != OutgoingPickSource::Original) {
    ImagePrepOptions options;
    options.max_pixels = limits.max_image_pixels;
    options.apply_exif_orientation = true;
    auto shrunk = PrepareAiImageFromFile(path, static_cast<size_t>(limits.max_image_bytes), kShrunkImageLongEdge, options);
    if (shrunk) {
      out.filename = StemOf(out.filename) + (shrunk->mime == "image/png" ? ".png" : ".jpg");
      out.mime = shrunk->mime;
      out.bytes = std::move(shrunk->bytes);
      out.image_is_shrunk = true;
      return out;
    }
    const AiImagePrepError why = AiImagePrepErrorOf(shrunk.error());
    if (why == AiImagePrepError::TooLarge) {
      return PrepError(OutgoingAttachmentError::ImageFailed, shrunk.error().message, Tr("chat.image.too_large"));
    }
    if (source == OutgoingPickSource::Photo) {
      return Unprocessable(); // it would have gone out with its metadata: not as a picture
    }
    // "File": the person chose to send this file, so it goes as the file it is.
    out.mime = "application/octet-stream";
  } else if (shrinkable) {
    // "Original": the picture as it is.
    out.mime = kind == ImageKind::Jpeg ? "image/jpeg" : kind == ImageKind::Png ? "image/png" : "image/webp";
    auto bytes = ReadAll(path, MaxAttachmentSendBytes(limits, out.mime, /*image_is_shrunk=*/false));
    if (!bytes) {
      return bytes.error();
    }
    if (kind == ImageKind::Jpeg) {
      auto cleaned = StripJpegLocation(*bytes);
      if (!cleaned) {
        return Unprocessable();
      }
      out.bytes = std::move(*cleaned);
    } else {
      out.bytes = std::move(*bytes);
    }
    return out;
  } else if (kind == ImageKind::Gif) {
    out.mime = "image/gif";
  } else if (named_as_picture || out.mime == "image/gif") {
    // Named as a picture but not one by content: never claimed to be an image.
    if (source != OutgoingPickSource::File) {
      return Unprocessable();
    }
    out.mime = "application/octet-stream";
  }

  auto bytes = ReadAll(path, MaxAttachmentSendBytes(limits, out.mime, /*image_is_shrunk=*/false));
  if (!bytes) {
    return bytes.error();
  }
  out.bytes = std::move(*bytes);
  return out;
}

} // namespace pbr
