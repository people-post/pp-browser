#pragma once

#include "feature/presenters/MessageForward.h"

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace pbr {

/**
 * The people picker's "choose and return" mode (DEV/PLAN-message-forward-text.md): it lists chats and people from
 * `list`, lets up to `max_targets` be ticked, and hands the ticked ones back. It invites, calls and creates nothing.
 * The words are the caller's (the picker reads no Tr() for this mode).
 */
/** One thing the strip above the buttons shows: a picture's thumbnail, a video's poster, or a type icon (dimmed when grey). */
struct ChoosePreviewItem {
  /** An icon under assets/ (always set: it shows while there is no thumbnail). */
  std::string icon;
  /** A picture file for an `<img>`; empty shows the icon. */
  std::string thumb_src;
  /** This device has nothing of it to send. */
  bool grey = false;
  /** A file's name and size, a voice message's length; empty for a picture or a video. */
  std::string label;
};

struct PeoplePickerChooseOptions {
  std::string title;
  /** The confirm button. */
  std::string send_label;
  /** The strip above the buttons: what is about to be sent. Empty: no strip. */
  std::string preview;
  /** Up to three things shown beside / under the words (attachments); empty shows the words only. */
  std::vector<ChoosePreviewItem> preview_items;
  /** A grey line under the strip ("2 aren't on this device and won't be forwarded"). */
  std::string preview_note;
  /**
   * Asked with how many chats are ticked each time that changes: the line that says why Send is off, or empty when it
   * may go. While it is not empty the line shows above the strip and Send is greyed.
   */
  std::function<std::string(size_t chats)> refusal;
  /** The bottom pill when one more than `max_targets` is tapped. */
  std::string limit_message;
  size_t max_targets = 9;
  /** What can be chosen, in the order to show it (the list is read when the picker opens). */
  std::function<std::vector<ForwardTarget>()> list;
};

/** The chosen ones, in the list's order. Runs after the picker is closed. */
using PeoplePickerChooseDone = std::function<void(std::vector<ForwardTarget> chosen)>;

} // namespace pbr
