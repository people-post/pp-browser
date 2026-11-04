#pragma once

#include <string>

namespace pbr {

/**
 * The share link of a relay file: `<origin>/api/relay/v1/files/<blob_id>/download?t=<token>`. The token is the
 * download credential (anyone holding the link can download until the file expires), so a link is never logged
 * whole: logs and errors carry the blob id or the redacted form only.
 */
struct RelayFileUrl {
  /** The blob id of a share link; "" when `url` is not one. */
  static std::string BlobIdOf(const std::string& url);
  /** `url` without its query and fragment (the token goes with the query). */
  static std::string Redacted(const std::string& url);
};

} // namespace pbr
