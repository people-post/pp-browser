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

TEST(AiImageAttachTest, BubbleShowsThumbnailOrMarker) {
  const std::string rml = R"(<div class="bubble bubble-user" selectable="text"><p class="bubble-text">[Image] What is this?</p></div>)";
  EXPECT_EQ(DecorateAiImageBubble(rml, "/tmp/a.jpg", 1200, 1600, "[Image]"),
            R"(<div class="bubble bubble-user" selectable="text"><img class="chat-ai-image" src="/tmp/a.jpg" style="width: 180dp; height: 240dp;"/><p class="bubble-text">What is this?</p></div>)");
  EXPECT_EQ(DecorateAiImageBubble(rml, "", 0, 0, "[图片]"),
            R"(<div class="bubble bubble-user" selectable="text"><p class="bubble-text">[图片] What is this?</p></div>)");
  // Only a user bubble that starts with the marker is touched.
  const std::string other = R"(<div class="bubble bubble-assistant"><p>[Image] not mine</p></div>)";
  EXPECT_EQ(DecorateAiImageBubble(other, "/tmp/a.jpg", 10, 10, "[Image]"), other);
}

TEST(AiImageAttachTest, BubbleImageFitsA240SquareKeepingItsShape) {
  EXPECT_EQ(AiImageBubbleSize(2048, 1024), (std::pair<int, int>{240, 120}));
  EXPECT_EQ(AiImageBubbleSize(600, 1800), (std::pair<int, int>{80, 240}));
  EXPECT_EQ(AiImageBubbleSize(100, 50), (std::pair<int, int>{100, 50})); // never enlarged
  EXPECT_EQ(AiImageBubbleSize(4000, 2), (std::pair<int, int>{240, 1}));
}
