#include "gui/chat/AiImageAttach.h"

#include <gtest/gtest.h>

using namespace pbr;

TEST(AiImageAttachTest, PeerThreadsKeepTheirOwnRule) {
  EXPECT_TRUE(ShowAttachButton({.peer_thread_attach = true}));
  EXPECT_FALSE(ShowAttachButton({.peer_thread_attach = false}));
  // The AI inputs do not matter for a peer thread.
  EXPECT_TRUE(ShowAttachButton({.peer_thread_attach = true, .brief_preset = false, .ai_usable = false}));
}

TEST(AiImageAttachTest, AiComposerNeedsBriefAndAUsableAi) {
  EXPECT_TRUE(ShowAttachButton({.ai_composer = true, .brief_preset = true, .ai_usable = true}));
  EXPECT_FALSE(ShowAttachButton({.ai_composer = true, .brief_preset = false, .ai_usable = true}));
  EXPECT_FALSE(ShowAttachButton({.ai_composer = true, .brief_preset = true, .ai_usable = false}));
  // The peer rule never turns the icon on in an AI composer.
  EXPECT_FALSE(ShowAttachButton({.peer_thread_attach = true, .ai_composer = true}));
}

TEST(AiImageAttachTest, EachFailureHasItsOwnMessage) {
  EXPECT_STREQ(AiImagePrepErrorKey(AiImagePrepError::NotAnImage), "chat.image.not_supported");
  EXPECT_STREQ(AiImagePrepErrorKey(AiImagePrepError::TooLarge), "chat.image.too_large");
  EXPECT_STREQ(AiImagePrepErrorKey(AiImagePrepError::Failed), "chat.image.read_failed");
  EXPECT_STREQ(AiImageErrorKindKey("image_too_large"), "chat.image.too_large");
  EXPECT_STREQ(AiImageErrorKindKey("image_unsupported"), "chat.image.not_supported");
  EXPECT_EQ(AiImageErrorKindKey(""), nullptr);
}

TEST(AiImageAttachTest, BubbleShowsThePictureFittedLikeAChatPictureOrTheMarker) {
  const std::string rml = R"(<div class="bubble bubble-user" selectable="text"><p class="bubble-text">[Image] What is this?</p></div>)";
  // Portrait: 292dp wide, cropped to 360dp tall, its shape kept (FitPictureInBubble); the bubble is the picture plus its 3dp frame.
  EXPECT_EQ(DecorateAiImageBubble(rml, "/tmp/a.jpg", 1200, 1600, "[Image]"),
            R"(<div class="bubble bubble-user bubble-user--picture" style="width: 298dp;" selectable="text"><div class="chat-ai-image-clip" style="width: 292dp; height: 360dp;"><img class="chat-ai-image" src="/tmp/a.jpg" width="292" height="389" style="margin-top: -14dp;"/></div><p class="bubble-text">What is this?</p></div>)");
  // Landscape (Kenneth 2026-11-07: the picture ran past the bubble's right edge): 280dp wide, never wider.
  EXPECT_EQ(DecorateAiImageBubble(rml, "/tmp/a.jpg", 1600, 1200, "[Image]"),
            R"(<div class="bubble bubble-user bubble-user--picture" style="width: 298dp;" selectable="text"><div class="chat-ai-image-clip" style="width: 292dp; height: 219dp;"><img class="chat-ai-image" src="/tmp/a.jpg" width="292" height="219"/></div><p class="bubble-text">What is this?</p></div>)");
  EXPECT_EQ(DecorateAiImageBubble(rml, "", 0, 0, "[图片]"),
            R"(<div class="bubble bubble-user" selectable="text"><p class="bubble-text">[图片] What is this?</p></div>)");
  // Only a user bubble that starts with the marker is touched.
  const std::string other = R"(<div class="bubble bubble-assistant"><p>[Image] not mine</p></div>)";
  EXPECT_EQ(DecorateAiImageBubble(other, "/tmp/a.jpg", 10, 10, "[Image]"), other);
}

// The overflow's cause: the bubble that ends a run carries the tail's host classes, so the old exact-class lookup
// never sized it and the picture stuck out. The picture bubble is found whatever else its class names.
TEST(AiImageAttachTest, ABubbleWithTheTailHostIsSizedToo) {
  const std::string rml =
      R"(<div class="bubble bubble-user bubble-tail-host bubble-tail-host--out" selectable="text"><svg class="bubble-tail"/><p class="bubble-text">[Image] Hi</p><div class="bubble-time">10:00</div></div>)";
  EXPECT_EQ(DecorateAiImageBubble(rml, "/tmp/a.jpg", 400, 400, "[Image]"),
            R"(<div class="bubble bubble-user bubble-tail-host bubble-tail-host--out bubble-user--picture" style="width: 298dp;" selectable="text"><svg class="bubble-tail"/><div class="chat-ai-image-clip" style="width: 292dp; height: 292dp;"><img class="chat-ai-image" src="/tmp/a.jpg" width="292" height="292"/></div><p class="bubble-text">Hi</p><div class="bubble-time">10:00</div></div>)");
}
