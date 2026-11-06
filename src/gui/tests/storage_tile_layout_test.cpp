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
  ui::CompiledGeometryHandle CompileGeometry(ui::Span<const ui::Vertex>, ui::Span<const int>) override { return 1; }
  void RenderGeometry(ui::CompiledGeometryHandle, ui::Vector2f, ui::TextureHandle) override {}
  void ReleaseGeometry(ui::CompiledGeometryHandle) override {}
  ui::TextureHandle LoadTexture(ui::Vector2i& dimensions, const ui::String&) override {
    dimensions = {1000, 3000};
    return 2;
  }
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

    for (int i = 0; i < 7; ++i) {
      items.push_back(Item(i, "image/png", "p" + std::to_string(i) + ".png"));
    }
    items.push_back(Item(50, "audio/mpeg", "a-very-long-recording-name-of-the-meeting.mp3"));
    StorageManagerPorts& ports = commands.storage;
    ports.list_stored = [this](std::function<void(std::vector<StoredAttachmentItem>)> done) { done(items); };
    ports.message_store_bytes = [](std::function<void(uint64_t)> done) { done(0); };
    ports.profile_bytes = [](std::function<void(uint64_t)> done) { done(0); };
    ports.chat_names = []() { return std::vector<StorageChatName>{{.thread_id = "a", .name = "A", .avatar_seed = "a"}}; };
    ports.thumbnail_path = [](const std::string&, const std::vector<uint8_t>&) { return std::string("tall.png"); };
    ports.run_later = [](std::chrono::milliseconds, std::function<void()> task) { task(); };
    navigation.push_local_back = [](const std::string&, std::function<void()>) {};
    navigation.clear_local_back = [](const std::string&) {};
    navigation.has_local_back = [](const std::string&) { return false; };
    controller = std::make_unique<StorageManagerController>(commands, navigation, feedback, []() { return true; });

    ASSERT_TRUE(DataModelHost::Instance().Register(context, "settings", [this](ui::DataModelConstructor& ctor) {
      controller->Bind(ctor);
    }));

    const std::string rml =
        "<rml><head><style>" + Asset("themes/components.rcss") + Asset("themes/colors-light.rcss") +
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
    if (context) { ui::RemoveContext(kContext); }
    if (initialized) { ui::Shutdown(); }
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
  std::vector<ui::Element*> tiles;  // the data-for templates stay in the DOM without the index attribute
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
    EXPECT_FLOAT_EQ(tile.top, first.top + static_cast<float>(i / 3) * 118.f) << "tile " << i << " starts right below the row above";

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

} // namespace
} // namespace pbr
