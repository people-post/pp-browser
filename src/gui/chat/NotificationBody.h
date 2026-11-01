#pragma once

#include <string>
#include <utility>

namespace pbr {

/** The banner text for an incoming message: the message itself, or `hidden_body` when the user hides content. */
inline std::string NotificationBodyFor(bool show_preview, std::string body, std::string hidden_body) {
  return show_preview ? std::move(body) : std::move(hidden_body);
}

} // namespace pbr
