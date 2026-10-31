#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace pbr {

/** What the head of an image file says, read without decoding the picture. */
struct ImageHeaderInfo {
  int width = 0;
  int height = 0;
  /** EXIF orientation 1..8 (JPEG only; 1: upright, also when absent). */
  int exif_orientation = 1;
};

/**
 * Size (and, for JPEG, EXIF orientation) of a PNG, JPEG, GIF or WebP file from its first bytes; nullopt for
 * any other format or a damaged head. Lets a caller refuse a huge picture before it is decoded.
 */
std::optional<ImageHeaderInfo> ReadImageHeader(const std::string& path);
/** The same for bytes already in memory (the head of the file is enough). */
std::optional<ImageHeaderInfo> ReadImageHeaderBytes(const std::vector<uint8_t>& head);

/**
 * True when a picture file is one this reader knows and its declared size is at most `max_pixels`: the check to
 * make before an image received from a contact is decoded for display (a header can claim 30000 x 30000).
 * Unknown formats and damaged heads are false.
 */
bool ImageWithinPixelCap(const std::string& path, uint64_t max_pixels);

/** What a file is by its first bytes, whatever its name says. */
enum class ImageKind { Unknown, Jpeg, Png, Gif, Webp };
ImageKind ImageKindOfBytes(const std::vector<uint8_t>& head);
ImageKind ImageKindOfFile(const std::string& path);

/**
 * A copy of a JPEG that keeps only what showing it needs: the tables, frame and scans, a JFIF header without
 * its thumbnail, an Adobe colour-transform segment and an ICC colour profile. Every other application segment
 * (EXIF with its GPS and camera data, XMP, IPTC, Multi-Picture / MPF, vendor data, a JFXX thumbnail), comments
 * and JPEG extensions are removed, and the file ends at its first end-of-image marker, so a second picture or a
 * trailer after it is gone. A non-upright orientation is kept as a minimal EXIF segment that holds nothing else.
 * Only this is claimed: it is not a promise that pixels carry nothing. Returns nullopt for anything that is not
 * a well-formed JPEG (not a JPEG, a damaged segment, no scan, no end of image): the caller must not send it on.
 */
std::optional<std::vector<uint8_t>> StripJpegLocation(const std::vector<uint8_t>& jpeg);

} // namespace pbr
