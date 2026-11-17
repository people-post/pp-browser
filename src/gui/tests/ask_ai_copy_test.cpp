// The ask-AI panel's copy: one bubble's plain text, and the whole conversation (every row, in order, one blank line
// between messages, no markup, no role labels).
#include "gui/chat/AskAiPanel.h"

#include <ui/core/Core.h>
#include <ui/dom/Context.h>
#include <ui/dom/ElementDocument.h>
#include <ui/paint/RenderInterface.h>
#include <gtest/gtest.h>

#include <string>

namespace pbr {
namespace {

class NullRender : public ui::RenderInterface {
public:
  ui::CompiledGeometryHandle CompileGeometry(ui::Span<const ui::Vertex>, ui::Span<const int>) override { return 1; }
  void RenderGeometry(ui::CompiledGeometryHandle, ui::Vector2f, ui::TextureHandle) override {}
  void ReleaseGeometry(ui::CompiledGeometryHandle) override {}
  ui::TextureHandle LoadTexture(ui::Vector2i&, const ui::String&) override { return 0; }
  ui::TextureHandle GenerateTexture(ui::Span<const ui::byte>, ui::Vector2i) override { return 1; }
  void ReleaseTexture(ui::TextureHandle) override {}
  void EnableScissorRegion(bool) override {}
  void SetScissorRegion(ui::Rectanglei) override {}
};

class AskAiCopyTest : public testing::Test {
protected:
  void SetUp() override {
    ui::SetRenderInterface(&renderer_);
    ASSERT_TRUE(ui::Initialise());
    context_ = ui::CreateContext("ask-ai-copy-test", {400, 800});
    ASSERT_NE(context_, nullptr);
    document_ = context_->LoadDocumentFromMemory(
        "<rml><head><style>body { width: 400px; }</style></head><body>"
        "<div id='ask-ai-scroll'>"
        "<p class='ask-ai-section'>Not a message</p>"
        "<div class='message-row ask-ai-row'><div selectable='text'>"
        "<div class='bubble bubble-user'><p class='bubble-text'>How do I add a friend &amp; a "
        "group?</p></div></div></div>"
        "<div class='message-row ask-ai-row'><div selectable='text'>"
        "<div class='bubble bubble-assistant'><p>Open <b>Contacts</b>,\n   then tap Add.</p>"
        "<ul><li>Friend</li><li>Group</li></ul></div></div></div>"
        "<p class='thinking'>Thinking</p>"
        "</div></body></rml>");
    ASSERT_NE(document_, nullptr);
    document_->Show();
    context_->Update();
  }
  void TearDown() override {
    ui::RemoveContext("ask-ai-copy-test");
    ui::Shutdown();
    ui::SetRenderInterface(nullptr);
  }

  NullRender renderer_;
  ui::Context* context_ = nullptr;
  ui::ElementDocument* document_ = nullptr;
};

TEST_F(AskAiCopyTest, OneBubbleCopiesItsOwnPlainText) {
  ui::ElementList rows;
  document_->GetElementsByClassName(rows, "ask-ai-row");
  ASSERT_EQ(rows.size(), 2u);
  EXPECT_EQ(AskAiPanel::PlainTextOf(rows[0]), "How do I add a friend & a group?");
  EXPECT_EQ(AskAiPanel::PlainTextOf(rows[1]), "Open Contacts, then tap Add.\nFriend\nGroup");
}

TEST_F(AskAiCopyTest, TheWholeConversationIsEveryRowInOrderWithABlankLineBetween) {
  const std::string all = AskAiPanel::ConversationText(document_->GetElementById("ask-ai-scroll"));
  EXPECT_EQ(all,
            "How do I add a friend & a group?\n\n"
            "Open Contacts, then tap Add.\nFriend\nGroup");
  EXPECT_EQ(all.find("Not a message"), std::string::npos);
  EXPECT_EQ(all.find("Thinking"), std::string::npos);
  EXPECT_EQ(all.find('<'), std::string::npos);
}

TEST_F(AskAiCopyTest, NoRowsCopiesNothing) {
  EXPECT_EQ(AskAiPanel::ConversationText(nullptr), "");
}

} // namespace
} // namespace pbr
