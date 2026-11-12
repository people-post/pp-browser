#pragma once

#include "lib/ui/ChatWidgetTypes.h"

#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

namespace pbr {

/**
 * The `src` of every chat picture (`<img class="chat-attachment-image">`: a picture's preview or the picture itself,
 * the thumbnail of a picture being sent) in the rows, unescaped, in row order; a file on two rows is there twice.
 */
std::vector<std::string> PictureSources(const std::vector<MessageDisplayRow>& rows);

/**
 * The name the UI library files an `<img src>` texture under, which ui::ReleaseTexture must be given: the library joins
 * the src with the document's path (SystemInterface::JoinPath), and for an absolute path that drops the leading '/'
 * ("/var/…/x.jpg" is filed as "var/…/x.jpg"; a Windows "C:\…" path stays as it is). Without a system interface:
 * the same rule by hand.
 */
std::string TextureKeyForImageSrc(const std::string& src);

/**
 * The chat pictures mounted in the page, counted per file (B203): the same picture sent twice in a chat is one file on
 * two rows, and its texture may go only when no row shows it. Mount / UnmountAll return the files no row shows any more,
 * for ui::ReleaseTexture once the page no longer has them.
 */
class ChatPictureTextures {
public:
  /** The rows the page has now. Returns the files that were on the page and are not any more. */
  std::vector<std::string> Mount(const std::vector<MessageDisplayRow>& rows);
  /** Nothing of the chat is drawn any more (left, locked, torn down): every file counted. */
  std::vector<std::string> UnmountAll();
  /** How many rows show `src` now (0: none). */
  size_t Count(const std::string& src) const;

private:
  std::unordered_map<std::string, size_t> counts_;
};

} // namespace pbr
