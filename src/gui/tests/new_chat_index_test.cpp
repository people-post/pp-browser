// The A-Z strip of the New chat page drives the New chat list, not the Contacts tab's list (the strip names its
// target). Real stylesheets over a page shaped like new_chat.rml and contacts.rml side by side.
#include "gui/contacts/ContactsController.h"

#include <ui/core/Core.h>
#include <ui/dom/Context.h>
#include <ui/dom/ElementDocument.h>
#include <ui/paint/RenderInterface.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
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
  ui::TextureHandle LoadTexture(ui::Vector2i& d, const ui::String&) override { d = {1, 1}; return 2; }
  ui::TextureHandle GenerateTexture(ui::Span<const ui::byte>, ui::Vector2i) override { return 1; }
  void ReleaseTexture(ui::TextureHandle) override {}
  void EnableScissorRegion(bool) override {}
  void SetScissorRegion(ui::Rectanglei) override {}
};

std::string Asset(const char* relative) {
  std::ifstream in(std::filesystem::path(PP_BROWSER_ASSETS_DIR) / relative, std::ios::binary);
  std::ostringstream out;
  out << in.rdbuf();
  return out.str();
}

std::vector<ContactsController::NewChatSectionRow> Sections() {
  std::vector<ContactsController::NewChatSectionRow> sections;
  for (char c = 'A'; c <= 'Z'; ++c) {
    ContactsController::NewChatSectionRow section;
    section.letter = ui::String(1, c);
    for (int i = 0; i < 3; ++i) {
      ContactsController::ContactListRow row;
      row.title = ui::String(1, c) + std::to_string(i);
      section.rows.push_back(row);
    }
    sections.push_back(section);
  }
  return sections;
}

TEST(NewChatIndexTest, TappingMScrollsTheNewChatListAndLeavesTheContactsListAlone) {
  NullRender renderer;
  ui::SetRenderInterface(&renderer);
  ASSERT_TRUE(ui::Initialise());
  ui::Context* context = ui::CreateContext("new-chat-index-test", {390, 844});
  ASSERT_NE(context, nullptr);
  context->ActivateTheme("light", true);

  const std::vector<ContactsController::NewChatSectionRow> sections = Sections();
  std::string nc_rows;
  std::string contacts_rows;
  for (const auto& section : sections) {
    nc_rows += "<p class=\"nc-section-header\">" + std::string(section.letter.c_str()) + "</p>";
    contacts_rows += "<p class=\"contacts-section-header\">" + std::string(section.letter.c_str()) + "</p>";
    for (int i = 0; i < 3; ++i) {
      nc_rows += "<div class=\"contacts-row\"></div>";
      contacts_rows += "<div class=\"contacts-row\"></div>";
    }
  }
  const std::string rml =
      "<rml><head><style>" + Asset("themes/foundation.rcss") + Asset("themes/components.rcss") +
      Asset("themes/colors-light.rcss") + "</style></head><body style=\"padding: 0;\">"
      "<div class=\"shell-layer-compact\">"
      "<div id=\"nc-panel\" style=\"position: absolute; top: 0dp; left: 0dp; width: 390dp; height: 700dp;\">"
      "<div class=\"nc-page\"><div class=\"nc-scroll\" id=\"nc-scroll\">" + nc_rows + "</div>"
      "<div class=\"contacts-index\" id=\"nc-strip\"></div></div></div>"
      "<div id=\"contacts-panel\" style=\"position: absolute; top: 0dp; left: 400dp; width: 390dp; height: 700dp;\">"
      "<div class=\"contacts-list-view\" id=\"contacts-scroll\" style=\"height: 700dp;\">" + contacts_rows + "</div>"
      "<div class=\"contacts-index\" id=\"contacts-strip\"></div></div>"
      "</div></body></rml>";
  ui::ElementDocument* document = context->LoadDocumentFromMemory(rml);
  ASSERT_NE(document, nullptr);
  document->Show();
  context->Update();
  context->Update();

  ui::Element* nc_scroll = document->GetElementById("nc-scroll");
  ui::Element* contacts_scroll = document->GetElementById("contacts-scroll");
  ASSERT_NE(nc_scroll, nullptr);
  ASSERT_NE(contacts_scroll, nullptr);
  ASSERT_GT(nc_scroll->Scroll().GetScrollHeight(), nc_scroll->GetClientHeight()) << "the list must scroll";
  EXPECT_FLOAT_EQ(nc_scroll->Scroll().GetScrollTop(), 0.f);

  ContactsController::ScrollIndexListTo(document->GetElementById("nc-strip"), "new_chat", sections, 'M');
  context->Update();

  EXPECT_GT(nc_scroll->Scroll().GetScrollTop(), 0.f) << "the New chat list moved";
  EXPECT_FLOAT_EQ(contacts_scroll->Scroll().GetScrollTop(), 0.f) << "the Contacts list did not";
  // The M header (13th) is now at the top of the list view.
  ui::ElementList headers;
  nc_scroll->QuerySelectorAll(headers, ".nc-section-header");
  ASSERT_EQ(headers.size(), 26u);
  const float m_top = headers[12]->GetAbsoluteOffset(ui::BoxArea::Border).y;
  const float list_top = nc_scroll->GetAbsoluteOffset(ui::BoxArea::Border).y;
  EXPECT_NEAR(m_top, list_top, 1.f);

  // And the other way round: the default target drives only the Contacts list.
  const float nc_before = nc_scroll->Scroll().GetScrollTop();
  ContactsController::ScrollIndexListTo(document->GetElementById("contacts-strip"), "contacts", sections, 'T');
  context->Update();
  EXPECT_GT(contacts_scroll->Scroll().GetScrollTop(), 0.f);
  EXPECT_FLOAT_EQ(nc_scroll->Scroll().GetScrollTop(), nc_before);

  ui::RemoveContext("new-chat-index-test");
  ui::Shutdown();
  ui::SetRenderInterface(nullptr);
}

} // namespace
} // namespace pbr
