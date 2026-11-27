#pragma once

#include "common/thread/ThreadRecordTypes.h"

namespace pbr {

/**
 * Which kinds of conversation take a reaction. A reaction is an annotation message sent to the peer(s)
 * (MeshDeliveryOrchestrator::SendReaction): a 1:1 chat and a group have someone to send it to; an AI conversation has
 * none (the AI would not see it), so no reaction is offered there: not in the message menu, not in the hover bar.
 */
inline bool ThreadTakesReactions(const ThreadKind kind) {
  return kind == ThreadKind::Direct || kind == ThreadKind::Group;
}

/**
 * What the message menu offers for a plain message, from the facts it decides on (ChatMenus::ForRequest); the wide
 * layout's hover bar draws exactly these slots, so the two cannot disagree. While a message is being edited, Reply and
 * Edit are not offered (each would replace the words being edited); reactions and Copy stay. More (the whole menu, with
 * Delete in it) is always there for a plain message.
 */
struct MessageMenuSlots {
  bool react = false; // the reaction row and its "+"
  bool reply = false;
  bool forward = false; // text and contact cards (ChatPresenter::CanForward); between Reply and Copy
  bool copy = false;
  bool edit = false;
};

inline MessageMenuSlots DecideMessageMenuSlots(const bool thread_takes_reactions, const bool has_text,
                                               const bool can_edit_now, const bool editing,
                                               const bool can_forward = false) {
  MessageMenuSlots slots;
  slots.react = thread_takes_reactions;
  slots.reply = has_text && !editing;
  slots.forward = can_forward;
  slots.copy = has_text;
  slots.edit = can_edit_now && !editing;
  return slots;
}

} // namespace pbr
