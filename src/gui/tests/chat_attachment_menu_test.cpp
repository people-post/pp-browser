// A received attachment's way out in the message menu (DEV/PLAN-received-file-save-as.md step 4): "Save as…" on a
// computer for every attachment but a voice message; nothing for a text message. ChatMenus::ForRequest needs a live
// facade, so the items are built by the function it calls (AppendAttachmentExitActions) and its call is read from the
// source.
#include "gui/chat/ChatMenus.h"

#include "foundation/i18n/LocalizationService.h"

#include <gtest/gtest.h>

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace pbr {
namespace {

/** The ids of the items a message menu gets for an attachment of `kind` (none: a text message). */
std::vector<std::string> ExitItems(const AttachmentLayout layout, const std::optional<AttachmentExitKind> kind,
                                   int* ran = nullptr) {
  std::vector<ContextMenuAction> actions;
  AppendAttachmentExitActions(actions, DecideMenuEntries(layout, kind), [ran]() {
    if (ran) {
      ++*ran;
    }
  });
  std::vector<std::string> ids;
  for (const ContextMenuAction& action : actions) {
    ids.push_back(action.id);
    if (ran && action.run) {
      action.run();
    }
  }
  return ids;
}

class ChatAttachmentMenuTest : public ::testing::Test {
protected:
  void SetUp() override {
    ASSERT_TRUE(LocalizationService::Instance().LoadFromAssets(PP_BROWSER_ASSETS_DIR));
    LocalizationService::Instance().SetPreferredLanguage("zh-Hans");
  }
  void TearDown() override { LocalizationService::Instance().SetPreferredLanguage("en"); }
};

TEST_F(ChatAttachmentMenuTest, TheDesktopOffersSaveAsForEveryReceivedAttachment) {
  for (const AttachmentExitKind kind : {AttachmentExitKind::Picture, AttachmentExitKind::Video,
                                        AttachmentExitKind::AudioFile, AttachmentExitKind::File}) {
    int ran = 0;
    EXPECT_EQ(ExitItems(AttachmentLayout::Desktop, kind, &ran), std::vector<std::string>{"save_attachment_as"});
    EXPECT_EQ(ran, 1) << "the item runs the save";
  }
  std::vector<ContextMenuAction> actions;
  AppendAttachmentExitActions(actions, DecideMenuEntries(AttachmentLayout::Desktop, AttachmentExitKind::File), {});
  ASSERT_EQ(actions.size(), 1u);
  EXPECT_EQ(actions[0].label, "存储为…");
}

TEST_F(ChatAttachmentMenuTest, NoSaveAsForATextOrVoiceMessageOrOnAPhone) {
  EXPECT_TRUE(ExitItems(AttachmentLayout::Desktop, std::nullopt).empty()) << "a text message";
  EXPECT_TRUE(ExitItems(AttachmentLayout::Desktop, AttachmentExitKind::Voice).empty()) << "an in-app voice message";
  EXPECT_TRUE(ExitItems(AttachmentLayout::Phone, AttachmentExitKind::File).empty());
}

std::string ReadSource(const std::string& file) {
  std::ifstream in(std::string(PP_SOURCE_DIR) + "/" + file, std::ios::binary);
  std::ostringstream out;
  out << in.rdbuf();
  return out.str();
}

// ForRequest puts the items after Copy / Edit and before Delete, from the attachment as its row was drawn.
TEST_F(ChatAttachmentMenuTest, TheMessageMenuAddsTheItemsBeforeDelete) {
  const std::string menus = ReadSource("src/gui/chat/ChatMenus.cpp");
  const size_t for_request = menus.find("std::vector<ContextMenuAction> ChatMenus::ForRequest(");
  ASSERT_NE(for_request, std::string::npos);
  const size_t edit = menus.find("InsertEditAction(actions,", for_request);
  const size_t drawn = menus.find("host_.DrawnAttachmentOf(message_id)", for_request);
  const size_t exit = menus.find("AppendAttachmentExitActions(", for_request);
  const size_t remove = menus.find("\"delete_message\"", for_request);
  ASSERT_NE(exit, std::string::npos) << "the message menu no longer offers the way out";
  EXPECT_LT(edit, exit);
  EXPECT_LT(drawn, exit);
  EXPECT_LT(exit, remove);
  EXPECT_NE(menus.find("DecideMenuEntries(CurrentAttachmentLayout()", for_request), std::string::npos);
}

} // namespace
} // namespace pbr
