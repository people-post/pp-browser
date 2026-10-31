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

bool IsShrinkableImage(const std::string& mime) {
  return mime == "image/png" || mime == "image/jpeg" || mime == "image/webp";
}

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
  std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
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
                                                  const bool original) {
  OutgoingAttachment out;
  out.filename = std::filesystem::path(path).filename().string();
  if (out.filename.empty()) {
    out.filename = "attachment";
  }
  out.mime = AttachmentMimeFromFilename(out.filename);

  if (IsShrinkableImage(out.mime) && !original) {
    ImagePrepOptions options;
    options.max_pixels = limits.max_image_pixels;
    options.apply_exif_orientation = true;
    auto shrunk = PrepareAiImageFromFile(path, static_cast<size_t>(limits.max_image_bytes), kShrunkImageLongEdge, options);
    if (shrunk) {
      const std::string stem = std::filesystem::path(out.filename).stem().string();
      out.filename = stem + (shrunk->mime == "image/png" ? ".png" : ".jpg");
      out.mime = shrunk->mime;
      out.bytes = std::move(shrunk->bytes);
      out.image_is_shrunk = true;
      return out;
    }
    if (AiImagePrepErrorOf(shrunk.error()) != AiImagePrepError::NotAnImage) {
      return PrepError(OutgoingAttachmentError::ImageFailed, shrunk.error().message,
                       Tr(AiImagePrepErrorOf(shrunk.error()) == AiImagePrepError::TooLarge ? "chat.image.too_large"
                                                                                           : "chat.image.read_failed"));
    }
    // Not a picture this device can decode: it goes as a file (a row with "Open"), not as a broken image.
    out.mime = "application/octet-stream";
  }

  auto bytes = ReadAll(path, MaxAttachmentSendBytes(limits, out.mime, /*image_is_shrunk=*/false));
  if (!bytes) {
    return bytes.error();
  }
  out.bytes = original && out.mime == "image/jpeg" ? StripJpegLocation(*bytes) : std::move(*bytes);
  return out;
}

} // namespace pbr
