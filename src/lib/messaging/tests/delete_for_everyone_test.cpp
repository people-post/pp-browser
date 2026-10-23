#include "lib/messaging/DeleteForEveryone.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

ThreadMessage Sent() {
  ThreadMessage message;
  message.sender_contact_id = kLocalSelfContactId;
  message.relay_visible = true;
  message.sender_seq = 7;
  message.text = "hi";
  return message;
}

TEST(DeleteForEveryoneTest, OnlyOurOwnMessagesThatWentOut) {
  EXPECT_TRUE(CanDeleteForEveryone(Sent()));
  ThreadMessage theirs = Sent();
  theirs.sender_contact_id = "contact-1";
  EXPECT_FALSE(CanDeleteForEveryone(theirs));
  ThreadMessage local_only = Sent();
  local_only.relay_visible = false; // e.g. an in-chat @ai question kept on this device
  EXPECT_FALSE(CanDeleteForEveryone(local_only));
  ThreadMessage unsent = Sent();
  unsent.sender_seq.reset();
  EXPECT_FALSE(CanDeleteForEveryone(unsent));
  for (const ChatContentType type : {ChatContentType::System, ChatContentType::Annotation, ChatContentType::Deleted}) {
    ThreadMessage other = Sent();
    other.content_type = type;
    EXPECT_FALSE(CanDeleteForEveryone(other));
  }
}

} // namespace
} // namespace pbr
