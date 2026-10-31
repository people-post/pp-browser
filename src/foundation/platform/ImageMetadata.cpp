#include "foundation/platform/ImageMetadata.h"

#include <cstring>
#include <fstream>
#include <iterator>

namespace pbr {

namespace {

/** How much of a file is read to find its size: JPEG metadata segments can be large, the rest is tiny. */
constexpr size_t kHeadBytes = 512 * 1024;

uint16_t Be16(const uint8_t* p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }
uint32_t Be32(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}
uint16_t Le16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t Le24(const uint8_t* p) { return static_cast<uint32_t>(p[0] | (p[1] << 8) | (p[2] << 16)); }

/** Orientation tag from the TIFF block of an Exif segment (`tiff`, `size` bytes); 1 when absent or odd. */
int ExifOrientation(const uint8_t* tiff, const size_t size) {
  if (size < 8) {
    return 1;
  }
  const bool big = tiff[0] == 'M' && tiff[1] == 'M';
  if (!big && !(tiff[0] == 'I' && tiff[1] == 'I')) {
    return 1;
  }
  const auto u16 = [&](const size_t at) { return big ? Be16(tiff + at) : Le16(tiff + at); };
  const auto u32 = [&](const size_t at) {
    return big ? Be32(tiff + at)
               : static_cast<uint32_t>(tiff[at] | (tiff[at + 1] << 8) | (tiff[at + 2] << 16) |
                                       (static_cast<uint32_t>(tiff[at + 3]) << 24));
  };
  const size_t ifd = u32(4);
  if (ifd + 2 > size) {
    return 1;
  }
  const size_t count = u16(ifd);
  for (size_t i = 0; i < count; ++i) {
    const size_t entry = ifd + 2 + i * 12;
    if (entry + 12 > size) {
      return 1;
    }
    if (u16(entry) == 0x0112 && u16(entry + 2) == 3) {
      const int value = u16(entry + 8);
      return value >= 1 && value <= 8 ? value : 1;
    }
  }
  return 1;
}

bool IsSofMarker(const uint8_t marker) {
  return marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC;
}

std::optional<ImageHeaderInfo> ReadJpegHeader(const std::vector<uint8_t>& d) {
  ImageHeaderInfo info;
  bool have_size = false;
  size_t pos = 2;
  while (pos + 4 <= d.size()) {
    if (d[pos] != 0xFF) {
      return std::nullopt;
    }
    const uint8_t marker = d[pos + 1];
    if (marker == 0xFF) {
      ++pos; // fill byte
      continue;
    }
    if (marker == 0xD8 || marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) {
      pos += 2;
      continue;
    }
    if (marker == 0xD9 || marker == 0xDA) {
      break; // end of image / start of the picture data
    }
    const size_t length = Be16(&d[pos + 2]);
    if (length < 2 || pos + 2 + length > d.size()) {
      break;
    }
    const uint8_t* body = &d[pos + 4];
    const size_t body_size = length - 2;
    if (IsSofMarker(marker) && body_size >= 5) {
      info.height = Be16(body + 1);
      info.width = Be16(body + 3);
      have_size = true;
    } else if (marker == 0xE1 && body_size > 14 && std::memcmp(body, "Exif\0\0", 6) == 0) {
      info.exif_orientation = ExifOrientation(body + 6, body_size - 6);
    }
    pos += 2 + length;
  }
  if (!have_size || info.width <= 0 || info.height <= 0) {
    return std::nullopt;
  }
  return info;
}

std::optional<ImageHeaderInfo> ReadPngHeader(const std::vector<uint8_t>& d) {
  if (d.size() < 24 || std::memcmp(&d[12], "IHDR", 4) != 0) {
    return std::nullopt;
  }
  ImageHeaderInfo info;
  info.width = static_cast<int>(Be32(&d[16]));
  info.height = static_cast<int>(Be32(&d[20]));
  if (info.width <= 0 || info.height <= 0) {
    return std::nullopt;
  }
  return info;
}

std::optional<ImageHeaderInfo> ReadGifHeader(const std::vector<uint8_t>& d) {
  if (d.size() < 10) {
    return std::nullopt;
  }
  ImageHeaderInfo info;
  info.width = Le16(&d[6]);
  info.height = Le16(&d[8]);
  if (info.width <= 0 || info.height <= 0) {
    return std::nullopt;
  }
  return info;
}

std::optional<ImageHeaderInfo> ReadWebpHeader(const std::vector<uint8_t>& d) {
  if (d.size() < 30) {
    return std::nullopt;
  }
  const uint8_t* chunk = &d[12];
  ImageHeaderInfo info;
  if (std::memcmp(chunk, "VP8X", 4) == 0) {
    info.width = static_cast<int>(Le24(chunk + 12) + 1);
    info.height = static_cast<int>(Le24(chunk + 15) + 1);
  } else if (std::memcmp(chunk, "VP8 ", 4) == 0) {
    if (chunk[11] != 0x9d || chunk[12] != 0x01 || chunk[13] != 0x2a) {
      return std::nullopt;
    }
    info.width = Le16(chunk + 14) & 0x3fff;
    info.height = Le16(chunk + 16) & 0x3fff;
  } else if (std::memcmp(chunk, "VP8L", 4) == 0) {
    if (chunk[8] != 0x2f) {
      return std::nullopt;
    }
    const uint32_t bits = static_cast<uint32_t>(chunk[9] | (chunk[10] << 8) | (chunk[11] << 16) |
                                                (static_cast<uint32_t>(chunk[12]) << 24));
    info.width = static_cast<int>((bits & 0x3fff) + 1);
    info.height = static_cast<int>(((bits >> 14) & 0x3fff) + 1);
  } else {
    return std::nullopt;
  }
  if (info.width <= 0 || info.height <= 0) {
    return std::nullopt;
  }
  return info;
}

/** APP1 "Exif" segment that only says the orientation. */
std::vector<uint8_t> MinimalOrientationExif(const int orientation) {
  std::vector<uint8_t> seg = {0xFF, 0xE1, 0x00, 0x22, 'E', 'x', 'i', 'f', 0, 0,
                              'M',  'M',  0x00, 0x2A, 0x00, 0x00, 0x00, 0x08,
                              0x00, 0x01, 0x01, 0x12, 0x00, 0x03, 0x00, 0x00, 0x00, 0x01,
                              0x00, static_cast<uint8_t>(orientation), 0x00, 0x00,
                              0x00, 0x00, 0x00, 0x00};
  return seg;
}

} // namespace

std::optional<ImageHeaderInfo> ReadImageHeaderBytes(const std::vector<uint8_t>& d) {
  static constexpr uint8_t kPng[] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  if (d.size() >= 3 && d[0] == 0xFF && d[1] == 0xD8 && d[2] == 0xFF) {
    return ReadJpegHeader(d);
  }
  if (d.size() >= 8 && std::memcmp(d.data(), kPng, 8) == 0) {
    return ReadPngHeader(d);
  }
  if (d.size() >= 6 && (std::memcmp(d.data(), "GIF87a", 6) == 0 || std::memcmp(d.data(), "GIF89a", 6) == 0)) {
    return ReadGifHeader(d);
  }
  if (d.size() >= 12 && std::memcmp(d.data(), "RIFF", 4) == 0 && std::memcmp(&d[8], "WEBP", 4) == 0) {
    return ReadWebpHeader(d);
  }
  return std::nullopt;
}

std::optional<ImageHeaderInfo> ReadImageHeader(const std::string& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return std::nullopt;
  }
  std::vector<uint8_t> head(kHeadBytes);
  input.read(reinterpret_cast<char*>(head.data()), static_cast<std::streamsize>(head.size()));
  head.resize(static_cast<size_t>(input.gcount()));
  return ReadImageHeaderBytes(head);
}

bool ImageWithinPixelCap(const std::string& path, const uint64_t max_pixels) {
  const auto header = ReadImageHeader(path);
  return header && static_cast<uint64_t>(header->width) * static_cast<uint64_t>(header->height) <= max_pixels;
}

ImageKind ImageKindOfBytes(const std::vector<uint8_t>& head) {
  static constexpr uint8_t kPng[] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  if (head.size() >= 3 && head[0] == 0xFF && head[1] == 0xD8 && head[2] == 0xFF) {
    return ImageKind::Jpeg;
  }
  if (head.size() >= 8 && std::memcmp(head.data(), kPng, 8) == 0) {
    return ImageKind::Png;
  }
  if (head.size() >= 6 && (std::memcmp(head.data(), "GIF87a", 6) == 0 || std::memcmp(head.data(), "GIF89a", 6) == 0)) {
    return ImageKind::Gif;
  }
  if (head.size() >= 12 && std::memcmp(head.data(), "RIFF", 4) == 0 && std::memcmp(&head[8], "WEBP", 4) == 0) {
    return ImageKind::Webp;
  }
  return ImageKind::Unknown;
}

ImageKind ImageKindOfFile(const std::string& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return ImageKind::Unknown;
  }
  std::vector<uint8_t> head(16);
  input.read(reinterpret_cast<char*>(head.data()), static_cast<std::streamsize>(head.size()));
  head.resize(static_cast<size_t>(input.gcount()));
  return ImageKindOfBytes(head);
}

std::optional<std::vector<uint8_t>> StripJpegLocation(const std::vector<uint8_t>& jpeg) {
  if (jpeg.size() < 4 || jpeg[0] != 0xFF || jpeg[1] != 0xD8 || jpeg[2] != 0xFF) {
    return std::nullopt;
  }
  std::vector<uint8_t> out = {0xFF, 0xD8};
  int orientation = 1;
  bool have_size = false;
  bool have_scan = false;
  size_t pos = 2;
  while (true) {
    if (pos + 2 > jpeg.size() || jpeg[pos] != 0xFF) {
      return std::nullopt; // damaged, or no end of image
    }
    const uint8_t marker = jpeg[pos + 1];
    if (marker == 0xFF) {
      ++pos; // fill byte
      continue;
    }
    if (marker == 0xD9) {
      if (!have_size || !have_scan) {
        return std::nullopt;
      }
      out.push_back(0xFF);
      out.push_back(0xD9);
      break; // everything after the first end of image (a second picture, a trailer) is dropped
    }
    if (marker == 0x00 || marker == 0xD8) {
      return std::nullopt;
    }
    if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) {
      out.push_back(0xFF); // a marker without a length
      out.push_back(marker);
      pos += 2;
      continue;
    }
    if (pos + 4 > jpeg.size()) {
      return std::nullopt;
    }
    const size_t length = Be16(&jpeg[pos + 2]);
    if (length < 2 || pos + 2 + length > jpeg.size()) {
      return std::nullopt;
    }
    const uint8_t* body = &jpeg[pos + 4];
    const size_t body_size = length - 2;
    const auto begin = jpeg.begin() + static_cast<std::ptrdiff_t>(pos);
    const auto end = begin + static_cast<std::ptrdiff_t>(2 + length);
    const auto is = [&](const char* tag, const size_t n) { return body_size >= n && std::memcmp(body, tag, n) == 0; };

    if (marker >= 0xE0 && marker <= 0xEF) {
      // Application segments: only what decoding or showing the picture right needs is kept.
      if (marker == 0xE0 && is("JFIF\0", 5) && body_size >= 14) {
        // The header without its embedded thumbnail (the thumbnail is a second, unstripped picture).
        std::vector<uint8_t> jfif = {0xFF, 0xE0, 0x00, 0x10};
        jfif.insert(jfif.end(), body, body + 12);
        jfif.push_back(0);
        jfif.push_back(0);
        out.insert(out.end(), jfif.begin(), jfif.end());
      } else if (marker == 0xEE && is("Adobe", 5)) {
        out.insert(out.end(), begin, end); // colour transform of the data
      } else if (marker == 0xE2 && is("ICC_PROFILE\0", 12)) {
        out.insert(out.end(), begin, end); // colour profile
      } else if (marker == 0xE1 && is("Exif\0\0", 6) && orientation == 1) {
        orientation = ExifOrientation(body + 6, body_size - 6);
      }
      pos += 2 + length;
      continue;
    }
    if (marker == 0xFE || (marker >= 0xF0 && marker <= 0xFD)) {
      pos += 2 + length; // comment, JPEG extensions
      continue;
    }
    if (IsSofMarker(marker)) {
      have_size = true;
    }
    out.insert(out.end(), begin, end);
    pos += 2 + length;
    if (marker == 0xDA) {
      // Scan data runs to the next real marker (stuffed FF00 and restart markers belong to it).
      have_scan = true;
      size_t scan_end = pos;
      while (scan_end + 1 < jpeg.size()) {
        if (jpeg[scan_end] == 0xFF) {
          const uint8_t next = jpeg[scan_end + 1];
          if (next == 0x00 || next == 0xFF || (next >= 0xD0 && next <= 0xD7)) {
            scan_end += next == 0xFF ? 1 : 2;
            continue;
          }
          break;
        }
        ++scan_end;
      }
      if (scan_end + 1 >= jpeg.size()) {
        return std::nullopt; // the scan never ends
      }
      out.insert(out.end(), jpeg.begin() + static_cast<std::ptrdiff_t>(pos),
                 jpeg.begin() + static_cast<std::ptrdiff_t>(scan_end));
      pos = scan_end;
    }
  }
  if (orientation != 1) {
    // Kept as the first segment after SOI (cameras write EXIF there too).
    const auto exif = MinimalOrientationExif(orientation);
    out.insert(out.begin() + 2, exif.begin(), exif.end());
  }
  return out;
}

} // namespace pbr
