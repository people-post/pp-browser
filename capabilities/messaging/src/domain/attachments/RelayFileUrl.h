#pragma once

#include <string>

namespace pbr {

/**
 * The share link of a relay file: `<origin>/api/relay/v1/blob/<blob_id>/download?t=<token>`, as the relay's
 * `blob/complete` answers it (`share_path`; the app keeps the link whole, it never builds one). A message sent before
 * the app moved to the `blob/` routes carries the legacy family, `/api/relay/v1/files/<blob_id>/download`: it is read the same
 * (the relay serves both until relay 1.0 removes the `files` family, RC018 item 5; a file lasts 7 days after `complete`,
 * and relay 1.0's deploy deletes the pre-release data, RC020 Q12).
 * The token is the download credential (anyone holding the link can download until the file expires), so a link is
 * never logged whole: logs and errors carry the blob id or the redacted form only.
 */
struct RelayFileUrl {
  /** The blob id of a share link (either family); "" when `url` is not one. */
  static std::string BlobIdOf(const std::string& url);
  /** `url` without its query and fragment (the token goes with the query). */
  static std::string Redacted(const std::string& url);
};

} // namespace pbr
