#pragma once

#include <string>

namespace pbr {

/**
 * The file name a picked path or Android content:// URI shows: the last path segment (after percent-decoding, and
 * after the "primary:" / "document/" prefixes of a documents-provider id) when it has a 1..8 character extension,
 * else empty (a media-store id such as ".../document/video%3A1234" carries no name; the caller then names the file
 * from its content). Never contains a directory separator.
 */
std::string DisplayNameFromPickedPath(const std::string& path_or_uri);

} // namespace pbr
