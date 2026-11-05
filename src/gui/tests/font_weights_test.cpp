// The bundled fonts in their weights, through the HarfBuzz font engine the app uses: Inter has four
// weights, and Chinese in medium / bold text gets a heavier face (fallback by weight, pp-cpp-ui).

#include "gui/shell/DeferredStartup.h"
#include "foundation/platform/IAssetLocator.h"

#include "FontEngineInterfaceHarfBuzz.h"
#include "FontFaceHandleHarfBuzz.h"

#include <ui/core/Core.h>
#include <ui/font/TextShapingContext.h>
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

ui::Style::FontWeight Weight(int weight) {
  return static_cast<ui::Style::FontWeight>(weight);
}

class FontWeightsTest : public testing::Test {
protected:
  void SetUp() override {
    ui::SetRenderInterface(&renderer_);
    ui::SetFontEngineInterface(&engine_);
    initialized_ = ui::Initialise();
    ASSERT_TRUE(initialized_);
  }

  void TearDown() override {
    if (initialized_) { ui::Shutdown(); }
    ui::SetRenderInterface(nullptr);
  }

  static std::string Asset(const char* relative) { return IAssetLocator::Instance().Resolve(relative); }

  ui::FontFaceHandle Handle(const char* family, int weight) {
    return engine_.GetFontFaceHandle(family, ui::Style::FontStyle::Normal, Weight(weight), 48);
  }

  int Width(const char* family, int weight, const std::string& text) {
    const ui::FontFaceHandle handle = Handle(family, weight);
    EXPECT_NE(handle, 0u);
    const ui::String language = "en";
    return handle ? engine_.GetStringWidth(handle, text, ui::TextShapingContext{language}, ui::Character::Null) : 0;
  }

  NullRender renderer_;
  FontEngineInterfaceHarfBuzz engine_;
  bool initialized_ = false;
};

TEST_F(FontWeightsTest, InterHasAFaceForEachWeight) {
  for (const char* file : {"fonts/Inter-Regular.ttf", "fonts/Inter-Medium.ttf", "fonts/Inter-SemiBold.ttf",
                           "fonts/Inter-Bold.ttf"}) {
    ASSERT_TRUE(ui::LoadFontFace(Asset(file)));
  }
  const std::string text = "Hamburgefonstiv";
  const int regular = Width("inter", 400, text);
  const int medium = Width("inter", 500, text);
  const int semibold = Width("inter", 600, text);
  const int bold = Width("inter", 700, text);
  EXPECT_LT(regular, medium);
  EXPECT_LT(medium, semibold);
  EXPECT_LT(semibold, bold);
}

TEST_F(FontWeightsTest, ChineseFamilyHasAFaceForEachWeight) {
  // Primary family without Latin or Chinese glyphs, in two weights, so every glyph comes from the fallback faces.
  ASSERT_TRUE(ui::LoadFontFace(Asset("fonts/NotoEmoji-Regular.ttf"), false, Weight(400)));
  ASSERT_TRUE(ui::LoadFontFace(Asset("fonts/NotoEmoji-Regular.ttf"), false, Weight(700)));
  ASSERT_TRUE(LoadChineseFonts());

  // One family, three faces.
  const ui::FontFaceHandle regular_face = Handle("noto sans cjk sc", 400);
  const ui::FontFaceHandle medium_face = Handle("noto sans cjk sc", 500);
  const ui::FontFaceHandle bold_face = Handle("noto sans cjk sc", 700);
  EXPECT_NE(regular_face, medium_face);
  EXPECT_NE(medium_face, bold_face);
  EXPECT_NE(regular_face, bold_face);

  // Chinese glyphs are all one em wide, so weigh the faces' Latin glyphs, which are wider when heavier.
  const std::string text = "Hamburgefonstiv";
  EXPECT_LT(Width("noto emoji", 400, text), Width("noto emoji", 700, text));
}

// Font engine internals: the glyph bitmap a run draws for `c`, through the fallback faces or the handle's own face.
const ui::byte* FallbackBitmap(ui::FontFaceHandle handle, ui::Character c) {
  const auto& glyphs = reinterpret_cast<FontFaceHandleHarfBuzz*>(handle)->GetFallbackGlyphs();
  const auto it = glyphs.find(c);
  return it == glyphs.end() ? nullptr : it->second.bitmap_data;
}
const ui::byte* OwnBitmap(ui::FontFaceHandle handle, ui::Character c) {
  for (const auto& [index, glyph] : reinterpret_cast<FontFaceHandleHarfBuzz*>(handle)->GetGlyphs()) {
    if (glyph.character == c) { return glyph.bitmap.bitmap_data; }
  }
  return nullptr;
}

TEST_F(FontWeightsTest, ChineseRunDrawsTheGlyphOfTheFaceForItsWeight) {
  ASSERT_TRUE(ui::LoadFontFace(Asset("fonts/NotoEmoji-Regular.ttf"), false, Weight(400)));
  ASSERT_TRUE(ui::LoadFontFace(Asset("fonts/NotoEmoji-Regular.ttf"), false, Weight(700)));
  ASSERT_TRUE(LoadChineseFonts());

  const std::string text = "\xE6\xB0\xB8";  // 永
  const auto c = static_cast<ui::Character>(0x6C38);
  // The face's own glyph in each weight of the Chinese family.
  for (const int weight : {400, 500, 700}) {
    ASSERT_GT(Width("noto sans cjk sc", weight, text), 0);
  }
  const ui::byte* regular = OwnBitmap(Handle("noto sans cjk sc", 400), c);
  const ui::byte* medium = OwnBitmap(Handle("noto sans cjk sc", 500), c);
  const ui::byte* bold = OwnBitmap(Handle("noto sans cjk sc", 700), c);
  ASSERT_NE(regular, nullptr);
  ASSERT_NE(medium, nullptr);
  ASSERT_NE(bold, nullptr);
  ASSERT_NE(regular, bold);

  // A run of 永 in text of each weight (Latin family without Chinese) draws the glyph of the matching face.
  ASSERT_TRUE(ui::LoadFontFace(Asset("fonts/NotoEmoji-Regular.ttf"), false, Weight(500)));
  ASSERT_TRUE(ui::LoadFontFace(Asset("fonts/NotoEmoji-Regular.ttf"), false, Weight(600)));
  for (const int weight : {400, 500, 600, 700}) {
    ASSERT_GT(Width("noto emoji", weight, text), 0);
  }
  EXPECT_EQ(FallbackBitmap(Handle("noto emoji", 400), c), regular);
  EXPECT_EQ(FallbackBitmap(Handle("noto emoji", 500), c), medium);
  EXPECT_EQ(FallbackBitmap(Handle("noto emoji", 600), c), bold);  // Tie between 500 and 700: the heavier.
  EXPECT_EQ(FallbackBitmap(Handle("noto emoji", 700), c), bold);
}

} // namespace
} // namespace pbr
