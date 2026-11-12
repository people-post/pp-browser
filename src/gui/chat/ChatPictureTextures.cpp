#include "gui/chat/ChatPictureTextures.h"

#include <ui/base/SystemInterface.h>

#include <string_view>
#include <utility>

namespace pbr {

std::vector<std::string> PictureSources(const std::vector<MessageDisplayRow>& rows) {
  static constexpr std::string_view kTag = "<img class=\"chat-attachment-image\" src=\"";
  static constexpr std::pair<std::string_view, char> kEntities[] = {
      {"&lt;", '<'}, {"&gt;", '>'}, {"&quot;", '"'}, {"&amp;", '&'}}; // StructuredTextParser::EscapeText's
  std::vector<std::string> out;
  for (const MessageDisplayRow& row : rows) {
    const std::string_view rml(row.content_rml.data(), row.content_rml.size());
    size_t at = 0;
    while ((at = rml.find(kTag, at)) != std::string_view::npos) {
      at += kTag.size();
      const size_t end = rml.find('"', at);
      if (end == std::string_view::npos) {
        break;
      }
      std::string src(rml.substr(at, end - at));
      for (const auto& [from, to] : kEntities) {
        for (size_t pos = 0; (pos = src.find(from, pos)) != std::string::npos; ++pos) {
          src.replace(pos, from.size(), 1, to);
        }
      }
      out.push_back(std::move(src));
      at = end;
    }
  }
  return out;
}

std::string TextureKeyForImageSrc(const std::string& src) {
  if (ui::SystemInterface* system = ui::GetSystemInterface()) {
    // The library's own join (RenderManager::LoadTexture): an absolute src does not depend on the document's path.
    ui::String key;
    system->JoinPath(key, ui::String(), src);
    return key;
  }
  return !src.empty() && src.front() == '/' ? src.substr(1) : src;
}

std::vector<std::string> ChatPictureTextures::Mount(const std::vector<MessageDisplayRow>& rows) {
  std::unordered_map<std::string, size_t> now;
  for (std::string& src : PictureSources(rows)) {
    ++now[std::move(src)];
  }
  std::vector<std::string> gone;
  for (const auto& [src, count] : counts_) {
    if (count > 0 && !now.contains(src)) {
      gone.push_back(src);
    }
  }
  counts_ = std::move(now);
  return gone;
}

std::vector<std::string> ChatPictureTextures::UnmountAll() {
  std::vector<std::string> gone;
  gone.reserve(counts_.size());
  for (const auto& [src, count] : counts_) {
    if (count > 0) {
      gone.push_back(src);
    }
  }
  counts_.clear();
  return gone;
}

size_t ChatPictureTextures::Count(const std::string& src) const {
  const auto found = counts_.find(src);
  return found == counts_.end() ? 0 : found->second;
}

} // namespace pbr
