#pragma once

#include <functional>
#include <string>

namespace pbr {

/**
 * Emoji-picker open hooks for ChatController (no EmojiPickerController::Instance()).
 * Application fills from EmojiPickerController. Clear via BindEmojiPickerNotify({}).
 */
struct EmojiPickerNotifyPorts {
  std::function<void()> open_insert;
  std::function<void(const std::string& message_id)> open_react;
};

} // namespace pbr
