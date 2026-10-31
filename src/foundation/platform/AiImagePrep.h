#pragma once

#include "common/Error.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

struct PreparedAiImage {
  std::vector<uint8_t> bytes;
  std::string mime; // "image/png" or "image/jpeg"
  int width = 0;    // dimensions of the encoded image
  int height = 0;
};

/** Failure kinds; the returned Error carries static_cast<int32_t>(kind) in Error::code. */
enum class AiImagePrepError : int32_t {
  NotAnImage = 1, // empty path, unreadable file, or a format SDL_image cannot decode
  TooLarge = 2,   // still above max_bytes after the smallest allowed re-encode
  Failed = 3,     // internal failure (allocation, scaling, encoding)
};

/** Extra rules for a picture that goes to a chat. The AI path uses none of them (the defaults). */
struct ImagePrepOptions {
  /** > 0: a picture with more pixels than this is refused (TooLarge) before it is decoded. Its size must be readable
   *  from the file head (PNG, JPEG, GIF, WebP); any other format is refused as NotAnImage. */
  uint64_t max_pixels = 0;
  /** Turn the picture upright by its EXIF orientation (the re-encode drops the tag). */
  bool apply_exif_orientation = false;
};

/**
 * Load an image file and re-encode it for sending to the AI.
 *
 * - The long edge is scaled to <= max_dimension (never upscaled). Animated formats yield the first frame.
 * - Surfaces with alpha are encoded as PNG; if that exceeds max_bytes they are flattened on white and encoded
 *   as JPEG. Everything else is JPEG.
 * - JPEG: qualities 85/70/55/40 are tried; if none fits, the dimensions are halved and it repeats, for at most
 *   4 rounds (down to 1/8 of the starting size).
 * Guarantee: on success bytes.size() <= max_bytes and the long edge <= max_dimension; otherwise an Error whose
 * code is an AiImagePrepError (NotAnImage vs TooLarge). Re-encoding drops all source metadata (EXIF/GPS).
 */
Roe<PreparedAiImage> PrepareAiImageFromFile(const std::string& path, size_t max_bytes = 1536u * 1024u,
                                            int max_dimension = 2048, const ImagePrepOptions& options = {});

/** The AiImagePrepError carried by an Error returned from PrepareAiImageFromFile. */
inline AiImagePrepError AiImagePrepErrorOf(const Error& err) {
  return static_cast<AiImagePrepError>(err.code);
}

} // namespace pbr
