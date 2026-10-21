#include "gui/chat/ChatTranscriptScroller.h"

#include <gtest/gtest.h>

using pbr::ChatTranscriptScroller;

// The on-screen keyboard shrinks the transcript viewport; a pinned transcript stays on its newest message.
TEST(ChatTranscriptViewportTest, PinnedStaysOnTheBottomWhenTheViewportShrinksOrGrows) {
  EXPECT_FLOAT_EQ(ChatTranscriptScroller::ScrollTopAfterViewportResize(600.f, 800.f, 500.f, 1400.f, true), 900.f);
  EXPECT_FLOAT_EQ(ChatTranscriptScroller::ScrollTopAfterViewportResize(900.f, 500.f, 800.f, 1400.f, true), 600.f);
  // Shorter than the viewport: nothing to scroll.
  EXPECT_FLOAT_EQ(ChatTranscriptScroller::ScrollTopAfterViewportResize(0.f, 800.f, 500.f, 300.f, true), 0.f);
}

// While reading older messages, what was in view stays in view: the offset moves by the height change.
TEST(ChatTranscriptViewportTest, UnpinnedKeepsTheContentInViewWithinRange) {
  EXPECT_FLOAT_EQ(ChatTranscriptScroller::ScrollTopAfterViewportResize(200.f, 800.f, 500.f, 1400.f, false), 500.f);
  EXPECT_FLOAT_EQ(ChatTranscriptScroller::ScrollTopAfterViewportResize(500.f, 500.f, 800.f, 1400.f, false), 200.f);
  // Clamped to the scrollable range at both ends.
  EXPECT_FLOAT_EQ(ChatTranscriptScroller::ScrollTopAfterViewportResize(50.f, 500.f, 800.f, 1400.f, false), 0.f);
  EXPECT_FLOAT_EQ(ChatTranscriptScroller::ScrollTopAfterViewportResize(850.f, 800.f, 500.f, 1400.f, false), 900.f);
}
