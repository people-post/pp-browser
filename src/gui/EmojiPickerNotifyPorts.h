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
  /** The same sheet for another field (the ask-AI panel's input): `insert` runs with each emoji picked. */
  std::function<void(std::function<void(const std::string& emoji)> insert)> open_insert_into;
  std::function<void(const std::string& message_id)> open_react;
  /** Closes the picker when it is open (search inside a chat takes the composer's place). */
  std::function<void()> close_if_open;
};

} // namespace pbr
