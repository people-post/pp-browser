// Every shipped stylesheet must parse without a single engine message. A rejected declaration ("Syntax error parsing
// property declaration ...") makes its rule silently not apply, on every platform; this test turns that into a failure.
#include <ui/base/SystemInterface.h>
#include <ui/core/Core.h>
#include <ui/dom/Context.h>
#include <ui/dom/ElementDocument.h>
#include <ui/paint/RenderInterface.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

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

/** Collects every error / warning the engine logs. */
class CapturingSystem : public ui::SystemInterface {
public:
  double GetElapsedTime() override { return 0.0; }
  bool LogMessage(ui::Log::Type type, const ui::String& message) override {
    if (type == ui::Log::LT_ERROR || type == ui::Log::LT_WARNING || type == ui::Log::LT_ASSERT) {
      messages.push_back(message);
    }
    return true;
  }
  std::vector<std::string> messages;
};

std::string ReadAll(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream out;
  out << in.rdbuf();
  std::string text = out.str();
  text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
  return text;
}

TEST(ThemeStylesheetParseTest, EveryShippedSheetParsesWithoutAnEngineMessage) {
  NullRender render;
  CapturingSystem system;
  ui::SetRenderInterface(&render);
  ui::SetSystemInterface(&system);
  ASSERT_TRUE(ui::Initialise());
  ui::Context* context = ui::CreateContext("theme-stylesheet-parse-test", {1280, 800});
  ASSERT_NE(context, nullptr);

  const char* const kSheets[] = {"base.rcss", "foundation.rcss",   "components.rcss",
                                 "wide.rcss", "colors-light.rcss", "colors-dark.rcss"};
  for (const char* sheet : kSheets) {
    system.messages.clear();
    const std::string css = ReadAll(std::string(PP_BROWSER_ASSETS_DIR) + "/themes/" + sheet);
    ASSERT_FALSE(css.empty()) << sheet;
    ui::ElementDocument* document =
        context->LoadDocumentFromMemory("<rml><head><style>" + css + "</style></head><body></body></rml>");
    ASSERT_NE(document, nullptr) << sheet;
    for (const std::string& message : system.messages) {
      ADD_FAILURE() << sheet << ": " << message;
    }
    document->Close();
  }

  ui::RemoveContext("theme-stylesheet-parse-test");
  ui::Shutdown();
  ui::SetSystemInterface(nullptr);
  ui::SetRenderInterface(nullptr);
}

} // namespace
} // namespace pbr
