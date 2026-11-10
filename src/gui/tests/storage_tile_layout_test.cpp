// The storage "All" grid on the phone: the real settings_section_storage.rml and stylesheets, a fake renderer that
// reports a tall (1000x3000) picture. Every tile must keep the same fixed box and clip the picture to its frame.
#include "gui/StorageManagerController.h"

#include "foundation/i18n/LocalizationService.h"
#include "gui/shell/DataModelHost.h"

#include <ui/core/Core.h>
#include <ui/dom/Context.h>
#include <ui/dom/ElementDocument.h>
#include <ui/paint/RenderInterface.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <fstream>
#include <sstream>
#include <string>

namespace pbr {
namespace {

class TallTextureRender : public ui::RenderInterface {
public:
  struct Draw {
    ui::TextureHandle texture;
    bool scissor;
    ui::Rectanglei region;
  };
  std::vector<Draw> draws; // every draw with the scissor in force at that moment

  ui::CompiledGeometryHandle CompileGeometry(ui::Span<const ui::Vertex>, ui::Span<const int>) override { return 1; }
  void RenderGeometry(ui::CompiledGeometryHandle, ui::Vector2f, ui::TextureHandle texture) override {
    draws.push_back({texture, scissor_on, scissor_region});
  }
  void ReleaseGeometry(ui::CompiledGeometryHandle) override {}
  ui::TextureHandle LoadTexture(ui::Vector2i& dimensions, const ui::String&) override {
    dimensions = {1000, 3000};
    return 2;
  }
  ui::TextureHandle GenerateTexture(ui::Span<const ui::byte>, ui::Vector2i) override { return 1; }
  void ReleaseTexture(ui::TextureHandle) override {}
  void EnableScissorRegion(bool enable) override { scissor_on = enable; }
  void SetScissorRegion(ui::Rectanglei region) override { scissor_region = region; }
  bool scissor_on = false;
  ui::Rectanglei scissor_region;
};

std::string ReadAll(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream out;
  out << in.rdbuf();
  std::string text = out.str();
  text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
  return text;
}

std::string Asset(const char* relative) {
  return ReadAll(std::string(PP_BROWSER_ASSETS_DIR) + "/" + relative);
}

StoredAttachmentItem Item(const int n, const std::string& mime, const std::string& filename) {
  StoredAttachmentItem item;
  item.thread_id = "a";
  item.message_id = "a-" + std::to_string(n);
  item.content_hash = std::vector<uint8_t>(32, static_cast<uint8_t>(n));
  item.mime = mime;
  item.filename = filename;
  item.bytes = 1024 * 1024;
  item.timestamp_ms = 1000 + n;
  return item;
}

constexpr int kWidth = 390;
constexpr int kHeight = 844;
constexpr const char* kContext = "storage-tile-layout-test";

class StorageTileLayoutTest : public testing::Test {
protected:
  void SetUp() override {
    ASSERT_TRUE(LocalizationService::Instance().LoadFromAssets(PP_BROWSER_ASSETS_DIR));
    LocalizationService::Instance().SetPreferredLanguage("en");
    ui::SetRenderInterface(&renderer);
    initialized = ui::Initialise();
    ASSERT_TRUE(initialized);
    context = ui::CreateContext(kContext, {kWidth, kHeight});
    ASSERT_NE(context, nullptr);
    context->ActivateTheme("light", true); // the colours live under @media (theme: light)

    for (int i = 0; i < 7; ++i) {
      items.push_back(Item(i, "image/png", "p" + std::to_string(i) + ".png"));
    }
    items.push_back(Item(50, "audio/mpeg", "a-very-long-recording-name-of-the-meeting.mp3"));
    StorageManagerPorts& ports = commands.storage;
    ports.list_stored = [this](std::function<void(std::vector<StoredAttachmentItem>)> done) { done(items); };
    ports.message_store_bytes = [](std::function<void(uint64_t)> done) { done(0); };
    ports.profile_bytes = [](std::function<void(uint64_t)> done) { done(0); };
    ports.chat_names = []() {
      return std::vector<StorageChatName>{{.thread_id = "a", .name = "A", .avatar_seed = "a"}};
    };
    // The sound file has no cover here (one comes from its chat card's tags): only the pictures get one.
    ports.thumbnail_path = [](const std::string&, const std::vector<uint8_t>& hash) {
      return hash.front() == 50 ? std::string() : std::string("tall.png");
    };
    ports.run_later = [](std::chrono::milliseconds, std::function<void()> task) { task(); };
    navigation.push_local_back = [](const std::string&, std::function<void()>) {};
    navigation.clear_local_back = [](const std::string&) {};
    navigation.has_local_back = [](const std::string&) { return false; };
    controller = std::make_unique<StorageManagerController>(commands, navigation, feedback, []() { return true; });

    ASSERT_TRUE(DataModelHost::Instance().Register(context, "settings",
                                                   [this](ui::DataModelConstructor& ctor) { controller->Bind(ctor); }));

    const std::string rml = "<rml><head><style>" + Asset("themes/foundation.rcss") + Asset("themes/components.rcss") +
                            Asset("themes/colors-light.rcss") +
                            "</style></head><body style=\"padding: 0;\"><div style=\"width: " + std::to_string(kWidth) +
                            "dp; padding: 0 16dp; box-sizing: border-box;\" data-model=\"settings\">" +
                            LocalizationService::Instance().LocalizeText(Asset("views/settings_section_storage.rml")) +
                            "</div></body></rml>";
    document = context->LoadDocumentFromMemory(rml);
    ASSERT_NE(document, nullptr);
    document->Show();
    context->Update();
    controller->Enter();
    controller->OpenManage();
    controller->OpenChat(0);
    DataModelHost::Instance().Dirty("settings", "storage_cells");
    DataModelHost::Instance().Dirty("settings", "storage_files");
    context->Update();
    context->Update();
  }
  void TearDown() override {
    DataModelHost::Instance().Clear();
    if (context) {
      ui::RemoveContext(kContext);
    }
    if (initialized) {
      ui::Shutdown();
    }
    ui::SetRenderInterface(nullptr);
  }
  struct Box {
    float left, top, width, height;
  };
  static Box BoxOf(ui::Element* element) {
    const ui::Vector2f at = element->GetAbsoluteOffset(ui::BoxArea::Border);
    const ui::Vector2f size = element->GetBox().GetSize(ui::BoxArea::Border);
    return {at.x, at.y, size.x, size.y};
  }
  std::vector<ui::Element*> ByClass(ui::Element* root, const char* cls) {
    ui::ElementList found;
    root->GetElementsByClassName(found, cls);
    return {found.begin(), found.end()};
  }

  TallTextureRender renderer;
  bool initialized = false;
  ui::Context* context = nullptr;
  ui::ElementDocument* document = nullptr;
  std::vector<StoredAttachmentItem> items;
  SettingsCommands commands;
  ShellNavigationPorts navigation;
  ShellFeedbackPorts feedback;
  std::unique_ptr<StorageManagerController> controller;
};

TEST_F(StorageTileLayoutTest, EveryTileKeepsOneFixedBoxAndAPortraitPictureIsClippedToIt) {
  ASSERT_EQ(controller->Cells().size(), 7u) << controller->Page() << " files " << controller->Files().size();
  std::vector<ui::Element*> tiles; // the data-for templates stay in the DOM without the index attribute
  for (ui::Element* tile : ByClass(document, "storage-tile")) {
    if (tile->HasAttribute("storage-cell") || tile->HasAttribute("storage-file")) {
      tiles.push_back(tile);
    }
  }
  ASSERT_EQ(tiles.size(), 8u) << "7 pictures and 1 file";
  const Box first = BoxOf(tiles[0]);
  ASSERT_GT(first.width, 50.f);
  EXPECT_FLOAT_EQ(first.height, 118.f);
  bool tall_image_seen = false;
  for (size_t i = 0; i < tiles.size(); ++i) {
    const Box tile = BoxOf(tiles[i]);
    EXPECT_FLOAT_EQ(tile.width, first.width) << "tile " << i;
    EXPECT_FLOAT_EQ(tile.height, 118.f) << "tile " << i << " must not grow with its picture";
    EXPECT_FLOAT_EQ(tile.left, first.left + static_cast<float>(i % 3) * first.width) << "tile " << i << " column";
    EXPECT_FLOAT_EQ(tile.top, first.top + static_cast<float>(i / 3) * 118.f)
        << "tile " << i << " starts right below the row above";

    const std::vector<ui::Element*> frames = ByClass(tiles[i], "storage-tile-frame");
    ASSERT_EQ(frames.size(), 1u);
    const Box frame = BoxOf(frames[0]);
    EXPECT_GE(frame.left, tile.left);
    EXPECT_GE(frame.top, tile.top);
    EXPECT_LE(frame.top + frame.height, tile.top + tile.height);
    EXPECT_LE(frame.left + frame.width, tile.left + tile.width);
    for (ui::Element* image : ByClass(tiles[i], "storage-tile-image")) {
      const Box img = BoxOf(image);
      EXPECT_GT(img.height, 2.f * frame.height) << "the 1000x3000 picture is laid out taller than its frame";
      tall_image_seen = true;
    }
  }
  EXPECT_TRUE(tall_image_seen);
}

// The layout boxes above only say where the picture is laid out; what reaches the screen is decided at paint time.
// A picture draw (the fake renderer's texture 2) must happen with the scissor on and inside its tile's frame.
TEST_F(StorageTileLayoutTest, ATallPictureIsPaintedUnderAScissorInsideItsTile) {
  renderer.draws.clear();
  context->Render();
  int pictures = 0;
  for (const auto& draw : renderer.draws) {
    if (draw.texture != 2) {
      continue;
    }
    ++pictures;
    ASSERT_TRUE(draw.scissor) << "a picture is drawn with no clip";
    EXPECT_EQ(draw.region.Height(), 116) << "the clip is the tile frame (118dp less 1dp each side)";
    EXPECT_LE(draw.region.Width(), static_cast<int>(kWidth / 3));
  }
  EXPECT_EQ(pictures, 7);
}

// The row under the chat title (All / Photos / Videos / Files): Kenneth's phone showed only an empty grey bar.
// Every tab needs text, a real box, a text colour that differs from the bar it sits on, and the chosen tab a filled pill.
TEST_F(StorageTileLayoutTest, TheCategoryTabsHaveTextAVisibleBoxAndAFilledSelectedPill) {
  std::vector<ui::Element*> bars = ByClass(document, "storage-seg");
  ASSERT_EQ(bars.size(), 1u);
  EXPECT_GE(BoxOf(bars[0]).height, 44.f) << "the row is a 44dp touch target";
  const std::vector<ui::Element*> tabs = ByClass(bars[0], "storage-seg-tab");
  ASSERT_EQ(tabs.size(), 4u);
  const auto same = [](const ui::Colourb a, const ui::Colourb b) {
    return a.red == b.red && a.green == b.green && a.blue == b.blue && a.alpha == b.alpha;
  };
  const ui::Colourb bar_bg = bars[0]->GetComputedValues().background_color();
  for (size_t i = 0; i < tabs.size(); ++i) {
    ui::Element* tab = tabs[i];
    const ui::Colourb text = tab->GetComputedValues().color();
    const ui::Colourb bg = tab->GetComputedValues().background_color();
    EXPECT_FALSE(tab->GetInnerRML().empty()) << "tab " << i << " has no label";
    EXPECT_GE(BoxOf(tab).height, 36.f) << "tab " << i;
    EXPECT_GT(BoxOf(tab).width, 30.f) << "tab " << i;
    EXPECT_GT(text.alpha, 200) << "tab " << i << " text is see-through";
    EXPECT_FALSE(same(text, bg)) << "tab " << i << " text colour equals its own background";
    EXPECT_FALSE(same(text, bar_bg)) << "tab " << i << " text colour equals the bar";
    if (i == 0) { // "All" is the chosen tab on open
      EXPECT_GT(bg.alpha, 200) << "the chosen tab is a filled pill";
    } else {
      EXPECT_EQ(bg.alpha, 0) << "the others sit flat on the bar";
    }
  }
}

} // namespace
} // namespace pbr
