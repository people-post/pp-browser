// The news reader sheet laid out on a 390dp phone with the real stylesheets and the shell's page-sheet markup
// (ShellLayoutRml::Overlays): the button row sits at the bottom of the sheet above the home-indicator strip, the title
// and the digest scroll above it without overlapping it, the three buttons are at least 44dp and are what a tap at
// their centre hits — for a short and a long digest, in English and in Chinese with a long title.
// (The end-to-end walk on 2026-11-23 found the row drawn over the title and Next not reachable: the view used the Add
// contact page's .ac-page / .ac-scroll, whose scroll is absolutely positioned over a compact layer.)
#include "foundation/i18n/LocalizationService.h"
#include "gui/chat/ChatDataModel.h"
#include "gui/shell/DataModelHost.h"
#include "lib/ai/MarkdownToRml.h"

#include <gtest/gtest.h>
#include <ui/core/Core.h>
#include <ui/dom/Context.h>
#include <ui/dom/ElementDocument.h>
#include <ui/paint/RenderInterface.h>

#include <algorithm>
#include <fstream>
#include <sstream>
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

std::string ReadAll(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream out;
  out << in.rdbuf();
  std::string text = out.str();
  text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
  return text;
}

std::string AssetPath(const char* relative) {
  return std::string(PP_BROWSER_ASSETS_DIR) + "/" + relative;
}

constexpr int kWidth = 390;
constexpr int kHeight = 844;
constexpr float kHomeStripDp = 34.f; // what ShellHost pads a phone page sheet's bottom with

struct Edges {
  float left, top, right, bottom;
  float MidX() const { return (left + right) / 2; }
  float MidY() const { return (top + bottom) / 2; }
  float Height() const { return bottom - top; }
};

class NewsReaderLayoutTest : public testing::Test {
protected:
  void SetUp() override {
    ui::SetRenderInterface(&renderer_);
    initialized_ = ui::Initialise();
    ASSERT_TRUE(initialized_);
    ASSERT_TRUE(ui::LoadFontFace(AssetPath("fonts/LatoLatin-Regular.ttf")));
    ASSERT_TRUE(ui::LoadFontFace(AssetPath("fonts/NotoSansSC-Medium.otf"), /*fallback_face=*/true));
    context_ = ui::CreateContext("news-reader-layout-test", {kWidth, kHeight});
    ASSERT_NE(context_, nullptr);
    ASSERT_TRUE(DataModelHost::Instance().Register(
        context_, "chat", [this](ui::DataModelConstructor& ctor) { BindChatScreenState(ctor, state_); }));
    ASSERT_TRUE(LocalizationService::Instance().LoadFromAssets(PP_BROWSER_ASSETS_DIR));
  }
  void TearDown() override {
    LocalizationService::Instance().SetPreferredLanguage("en");
    DataModelHost::Instance().Clear();
    if (context_) {
      ui::RemoveContext("news-reader-layout-test");
    }
    if (initialized_) {
      ui::Shutdown();
    }
    ui::SetRenderInterface(nullptr);
  }

  /** The sheet as ShellLayoutRml::Overlays writes it, with the reader view inside. */
  void Show(const std::string& language) {
    LocalizationService::Instance().SetPreferredLanguage(language);
    const std::string view = LocalizationService::Instance().LocalizeText(ReadAll(AssetPath("views/news_reader.rml")));
    const std::string rml =
        "<rml><head><style>" + ReadAll(AssetPath("themes/foundation.rcss")) +
        ReadAll(AssetPath("themes/components.rcss")) + ReadAll(AssetPath("themes/colors-light.rcss")) +
        "</style></head><body style=\"padding: 0; font-size: 15dp;\">"
        "<div style=\"position: absolute; top: 0; left: 0; width: " +
        std::to_string(kWidth) + "dp; height: " + std::to_string(kHeight) +
        "dp;\"><div class=\"shell-layer shell-layer-overlay shell-layer-sheet\"><div class=\"shell-scrim\"></div>"
        "<div class=\"shell-page-sheet shell-page-sheet--news_reader shell-page-sheet--large\" id=\"sheet\" "
        "style=\"padding-bottom: " +
        std::to_string(static_cast<int>(kHomeStripDp)) +
        "dp;\"><div class=\"shell-page-sheet-grabber\"></div>"
        "<button class=\"shell-close-btn shell-page-sheet-close\" type=\"button\"><span class=\"sheet-icon-disc\">"
        "</span></button><div class=\"shell-overlay-body shell-layer-compact\">" +
        view + "</div></div></div></div></body></rml>";
    document_ = context_->LoadDocumentFromMemory(rml);
    ASSERT_NE(document_, nullptr);
    document_->Show();
    for (int i = 0; i < 3; ++i) {
      context_->Update();
    }
  }

  void Fill(const std::string& title, const std::string& digest_markdown) {
    state_.news_reader_title = title.c_str();
    state_.news_reader_body_rml = MarkdownToRml(digest_markdown, MarkdownOptions{.links = false}).rml.c_str();
    state_.news_reader_has_nav = true;
    state_.news_reader_can_prev = true;
    state_.news_reader_can_next = true;
  }

  static Edges BorderEdges(ui::Element* element) {
    const ui::Vector2f at = element->GetAbsoluteOffset(ui::BoxArea::Border);
    const ui::Vector2f size = element->GetBox().GetSize(ui::BoxArea::Border);
    return {at.x, at.y, at.x + size.x, at.y + size.y};
  }
  std::vector<ui::Element*> All(const char* cls) {
    ui::ElementList found;
    document_->GetElementsByClassName(found, cls);
    return {found.begin(), found.end()};
  }
  ui::Element* First(const char* cls) {
    const std::vector<ui::Element*> found = All(cls);
    return found.empty() ? nullptr : found[0];
  }
  /** A tap at the centre of `element` lands on it (or inside it). */
  bool HitsItsCentre(ui::Element* element) {
    const Edges e = BorderEdges(element);
    for (ui::Element* hit = context_->GetElementAtPoint({e.MidX(), e.MidY()}); hit; hit = hit->GetParentNode()) {
      if (hit == element) {
        return true;
      }
    }
    return false;
  }

  /** Everything the walk found wrong, checked for the sheet now shown. */
  void ExpectTheBarAtTheBottomAndTappable() {
    ui::Element* sheet = document_->GetElementById("sheet");
    ui::Element* bar = First("news-reader-bar");
    ui::Element* title = First("news-reader-title");
    ui::Element* scroll = First("news-reader-scroll");
    ASSERT_NE(sheet, nullptr);
    ASSERT_NE(bar, nullptr);
    ASSERT_NE(title, nullptr);
    ASSERT_NE(scroll, nullptr);
    const Edges s = BorderEdges(sheet);
    const Edges b = BorderEdges(bar);
    const Edges t = BorderEdges(title);
    const Edges body = BorderEdges(scroll);
    EXPECT_GT(t.Height(), 20.f) << "the title is laid out";
    EXPECT_NEAR(b.bottom, s.bottom - kHomeStripDp, 1.f) << "the row sits at the sheet's bottom, above the home strip";
    EXPECT_LE(b.bottom, static_cast<float>(kHeight) + 0.5f);
    EXPECT_LE(body.bottom, b.top + 0.5f) << "the scrolling part ends where the row begins";
    if (t.bottom <= body.bottom) { // the title is in view (not scrolled away): never under the row
      EXPECT_LE(t.bottom, b.top + 0.5f) << "title " << t.top << ".." << t.bottom << " row " << b.top;
    }
    EXPECT_LE(t.right, s.right) << "the title wraps inside the sheet";
    const std::vector<ui::Element*> steps = All("news-reader-step");
    ASSERT_EQ(steps.size(), 2u);
    ui::Element* open = First("news-reader-open");
    ASSERT_NE(open, nullptr);
    for (ui::Element* button : {steps[0], open, steps[1]}) {
      const Edges e = BorderEdges(button);
      EXPECT_GE(e.Height(), 44.f);
      EXPECT_GE(e.right - e.left, 44.f);
      EXPECT_GE(e.top, b.top - 0.5f);
      EXPECT_LE(e.bottom, b.bottom + 0.5f);
      EXPECT_TRUE(HitsItsCentre(button)) << "a tap at its centre reaches it";
    }
    EXPECT_LT(BorderEdges(steps[0]).right, BorderEdges(open).left) << "Previous, Open original, Next in a row";
    EXPECT_LT(BorderEdges(open).right, BorderEdges(steps[1]).left);
  }

  NullRender renderer_;
  bool initialized_ = false;
  ui::Context* context_ = nullptr;
  ui::ElementDocument* document_ = nullptr;
  ChatScreenState state_;
};

std::string LongDigest() {
  std::string text = "The Hindu · November 23, 2026\n\n";
  for (int i = 0; i < 30; ++i) {
    text +=
        "The parties concluded a new round of talks without reaching an agreement on a coalition, which would "
        "require concessions from both sides.\n\n";
  }
  return text;
}

TEST_F(NewsReaderLayoutTest, TheRowSitsAtTheBottomUnderAShortDigest) {
  Fill("A Russian cargo ship remains on fire off Finland", "Economic Times\n\nThe ship is still burning.");
  Show("en");
  ExpectTheBarAtTheBottomAndTappable();
}

TEST_F(NewsReaderLayoutTest, TheRowSitsAtTheBottomUnderALongDigest) {
  Fill("Netanyahu, Eisenkot parties end talks without deal on forming Israel government", LongDigest());
  Show("en");
  ExpectTheBarAtTheBottomAndTappable();
}

TEST_F(NewsReaderLayoutTest, WhileLoadingTheRowStillSitsAtTheBottom) {
  Fill("A Russian cargo ship remains on fire off Finland", "");
  state_.news_reader_body_rml = "";
  state_.news_reader_loading = true;
  Show("en");
  ExpectTheBarAtTheBottomAndTappable();
  ui::Element* loading = First("news-reader-loading");
  ASSERT_NE(loading, nullptr);
  EXPECT_LE(BorderEdges(loading).bottom, BorderEdges(First("news-reader-bar")).top);
}

// A translated digest: the "AI translation of the original" line sits under a long title, above the digest, and nothing
// overlaps the row.
TEST_F(NewsReaderLayoutTest, TheTranslationLineSitsUnderALongTitle) {
  const std::string title =
      "以色列总理内塔尼亚胡与反对派领袖艾森科特的政党结束新一轮组阁谈判但未能达成任何协议各方仍在继续接触";
  Fill(title, "来源：Reuters\n\n译文说，双方在周日结束了新一轮谈判。");
  state_.news_reader_translated = true;
  Show("zh-Hans");
  ExpectTheBarAtTheBottomAndTappable();
  ui::Element* line = First("news-reader-translated");
  ASSERT_NE(line, nullptr);
  ASSERT_TRUE(line->IsVisible());
  EXPECT_NE(std::string(line->GetInnerRML().c_str()).find("AI 翻译自原文，细节以原文为准"), std::string::npos);
  const Edges t = BorderEdges(First("news-reader-title"));
  const Edges l = BorderEdges(line);
  const Edges body = BorderEdges(First("news-reader-body"));
  EXPECT_GT(t.Height(), 40.f) << "the long title wraps";
  EXPECT_GE(l.top, t.bottom - 0.5f - 6.f) << "under the title";
  EXPECT_GT(l.top, t.top + t.Height() / 2) << "under the title";
  EXPECT_LE(l.bottom, body.top + 0.5f) << "above the digest";
  EXPECT_GT(l.Height(), 10.f);

  // An untranslated digest: no line.
  state_.news_reader_translated = false;
  DataModelHost::Instance().Dirty("chat", "news_reader_translated");
  for (int i = 0; i < 3; ++i) {
    context_->Update();
  }
  EXPECT_FALSE(First("news-reader-translated")->IsVisible());
}

// The walk could not type Chinese: the zh-Hans strings and a long Chinese title (it wraps, nothing overlaps).
TEST_F(NewsReaderLayoutTest, ChineseStringsAndALongChineseTitleWrapWithoutOverlap) {
  const std::string title =
      "以色列总理内塔尼亚胡与反对派领袖艾森科特的政党结束新一轮组阁谈判但未能达成任何协议各方仍在继续接触";
  ASSERT_GE(title.size() / 3, 40u);
  Fill(title, "来源：新华网 · 2026年11月23日\n\n文章说，双方在周日结束了新一轮谈判，没有达成联合政府协议。");
  Show("zh-Hans");
  ExpectTheBarAtTheBottomAndTappable();
  EXPECT_GT(BorderEdges(First("news-reader-title")).Height(), 40.f) << "the long title wraps onto more lines";
  EXPECT_NE(std::string(First("news-reader-open")->GetInnerRML().c_str()).find("打开原文"), std::string::npos);
  EXPECT_NE(std::string(All("news-reader-step")[1]->GetInnerRML().c_str()).find("下一条"), std::string::npos);

  // Loading and the failure line in Chinese sit above the row too.
  state_.news_reader_body_rml = "";
  state_.news_reader_failed = true;
  DataModelHost::Instance().Dirty("chat", "news_reader_body_rml");
  DataModelHost::Instance().Dirty("chat", "news_reader_failed");
  for (int i = 0; i < 3; ++i) {
    context_->Update();
  }
  ui::Element* failed = nullptr;
  for (ui::Element* note : All("news-reader-note")) {
    if (note->IsVisible() &&
        std::string(note->GetInnerRML().c_str()).find("这篇暂时取不到内容。") != std::string::npos) {
      failed = note;
    }
  }
  ASSERT_NE(failed, nullptr);
  EXPECT_LE(BorderEdges(failed).bottom, BorderEdges(First("news-reader-bar")).top);
}

} // namespace
} // namespace pbr
