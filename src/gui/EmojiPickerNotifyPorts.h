#pragma once

#include <functional>
#include <string>

namespace ui {
class Element;
}

namespace pbr {

/**
 * Emoji-picker open hooks for ChatController (no EmojiPickerController::Instance()).
 * Application fills from EmojiPickerController. Clear via BindEmojiPickerNotify({}).
 */
struct EmojiPickerNotifyPorts {
  std::function<void()> open_insert;
  std::function<void(const std::string& message_id)> open_react;
  /** The phone message sheet's "+": every emoji inside the sheet (`mount`); `done` closes the sheet after a pick. */
  std::function<void(ui::Element* mount, const std::string& message_id, std::function<void()> done)> open_react_in;
  /** Closes the picker when it is open (search inside a chat takes the composer's place). */
  std::function<void()> close_if_open;
};

} // namespace pbr
