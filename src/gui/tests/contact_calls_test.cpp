// DEV/PLAN-fewer-taps-1.md 2 and 3: the chat header opens the person's page, the contact page has voice and video
// tiles, and a person who is not a contact gets copy-ID and Block on the member page.
#include "gui/chat/ChatPeerSheet.h"
#include "gui/contacts/ContactsController.h"

#include "common/directory/DirectoryJson.h"
#include "foundation/i18n/LocalizationService.h"
#include "lib/people/ContactTypes.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace pbr {
namespace {

std::string ReadAsset(const std::string& relative) {
  std::ifstream in(std::filesystem::path(PP_BROWSER_ASSETS_DIR) / relative);
  std::stringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

// --- what the chat header's person opens ---------------------------------------------------------------------------

TEST(ChatPeerTargetTest, PhoneOneToOneOpensThePersonsPage) {
  EXPECT_EQ(ChooseChatPeerTarget(true, ThreadKind::Direct, /*is_contact=*/true, true), ChatPeerTarget::ContactPage);
  EXPECT_EQ(ChooseChatPeerTarget(true, ThreadKind::Direct, true, false), ChatPeerTarget::ContactPage)
      << "a contact opens whatever kind of id the chat has";
  EXPECT_EQ(ChooseChatPeerTarget(true, ThreadKind::Direct, /*is_contact=*/false, /*peer_is_account=*/true),
            ChatPeerTarget::MemberPage);
}

TEST(ChatPeerTargetTest, AStrangerWhoseChatIdIsNotAnAccountKeepsTheMenu) {
  EXPECT_EQ(ChooseChatPeerTarget(true, ThreadKind::Direct, false, false), ChatPeerTarget::Menu);
}

TEST(ChatPeerTargetTest, GroupsAiAndTheDesktopKeepTheMenu) {
  for (const bool contact : {false, true}) {
    for (const bool account : {false, true}) {
      EXPECT_EQ(ChooseChatPeerTarget(true, ThreadKind::Group, contact, account), ChatPeerTarget::Menu);
      EXPECT_EQ(ChooseChatPeerTarget(true, ThreadKind::Ai, contact, account), ChatPeerTarget::Menu);
      EXPECT_EQ(ChooseChatPeerTarget(false, ThreadKind::Direct, contact, account), ChatPeerTarget::Menu);
      EXPECT_EQ(ChooseChatPeerTarget(false, ThreadKind::Group, contact, account), ChatPeerTarget::Menu);
    }
  }
}

// The three dots' first item "Add people…" is offered exactly when the header opens a contact's page (the menu
// asks ChooseChatPeerTarget with the same inputs), so phone + 1:1 + contact only; the desktop's popup still has it.
TEST(ChatPeerTargetTest, AddPeopleMovesToTheThreeDotsOnlyWhereTheHeaderOpensTheContactPage) {
  const auto offered = [](bool compact, ThreadKind kind, bool contact) {
    return ChooseChatPeerTarget(compact, kind, contact, false) == ChatPeerTarget::ContactPage;
  };
  EXPECT_TRUE(offered(true, ThreadKind::Direct, true));
  EXPECT_FALSE(offered(true, ThreadKind::Direct, false)) << "not a contact: nobody to add people through";
  EXPECT_FALSE(offered(false, ThreadKind::Direct, true)) << "desktop: the popup menu has it";
  EXPECT_FALSE(offered(true, ThreadKind::Group, true));
}

// --- the pages ------------------------------------------------------------------------------------------------------

TEST(ContactPageTilesTest, FourTilesInOrderAndTheCallTilesFollowMessageAndNotBlocked) {
  const std::string rml = ReadAsset("views/contact_detail.rml");
  const size_t row = rml.find("class=\"cd-acts cd-acts--tiles row\"");
  ASSERT_NE(row, std::string::npos);
  const size_t message = rml.find("data-event-click=\"start_chat()\"", row);
  const size_t voice = rml.find("data-event-click=\"call_contact_voice()\"", row);
  const size_t video = rml.find("data-event-click=\"call_contact_video()\"", row);
  const size_t star = rml.find("data-event-click=\"toggle_star()\"", row);
  ASSERT_NE(message, std::string::npos);
  ASSERT_NE(voice, std::string::npos);
  ASSERT_NE(video, std::string::npos);
  ASSERT_NE(star, std::string::npos);
  EXPECT_LT(message, voice);
  EXPECT_LT(voice, video);
  EXPECT_LT(video, star);
  EXPECT_NE(rml.find("data-if=\"selected.can_message && selected.trust_key != 'blocked'\"\n"
                     "                data-event-click=\"call_contact_voice()\""),
            std::string::npos);
  EXPECT_NE(rml.find("data-if=\"selected.can_message && selected.trust_key != 'blocked'\"\n"
                     "                data-event-click=\"call_contact_video()\""),
            std::string::npos);
  EXPECT_NE(rml.find("{{i18n:call.start.voice}}"), std::string::npos);
  EXPECT_NE(rml.find("{{i18n:call.start.video}}"), std::string::npos);
  // The member page (not a contact) has no call tiles: its markup ends before the contact's.
  const size_t member_end = rml.find("<div data-if=\"!selected.member_preview\">");
  ASSERT_NE(member_end, std::string::npos);
  EXPECT_GT(voice, member_end);
}

TEST(ContactPageTilesTest, TheTilesShareTheRowEqually) {
  const std::string css = ReadAsset("themes/components.rcss");
  const size_t row = css.find(".cd-acts--tiles {");
  ASSERT_NE(row, std::string::npos);
  const std::string block = css.substr(row, css.find('}', row) - row);
  EXPECT_NE(block.find("gap: 8dp;"), std::string::npos);
  EXPECT_NE(block.find("margin: 16dp 16dp 0 16dp;"), std::string::npos);
  const size_t tile = css.find(".cd-acts--tiles .cd-act {");
  ASSERT_NE(tile, std::string::npos);
  const std::string tile_block = css.substr(tile, css.find('}', tile) - tile);
  EXPECT_NE(tile_block.find("flex: 1 1 0;"), std::string::npos);
  EXPECT_NE(tile_block.find("height: 64dp;"), std::string::npos);
}

TEST(MemberPageRowsTest, ACopyIdRowAndABlockRowOnThePageOfSomeoneWhoIsNotAContact) {
  const std::string rml = ReadAsset("views/contact_detail.rml");
  const size_t member = rml.find("<div data-if=\"selected.member_preview\">");
  const size_t contact = rml.find("<div data-if=\"!selected.member_preview\">");
  ASSERT_NE(member, std::string::npos);
  ASSERT_LT(member, contact);
  const std::string page = rml.substr(member, contact - member);
  EXPECT_NE(page.find("data-event-click=\"copy_contact_id()\""), std::string::npos);
  EXPECT_NE(page.find("data-event-click=\"block_member()\""), std::string::npos);
  EXPECT_NE(page.find("{{i18n:contacts.block}}"), std::string::npos);
  EXPECT_NE(page.find("data-event-click=\"add_member_contact()\""), std::string::npos) << "Add stays";
}

TEST(ContactPageRowsTest, TheContactPageAlreadyHasCopyIdBlockUnblockAndRemove) {
  const std::string rml = ReadAsset("views/contact_detail.rml");
  const std::string page = rml.substr(rml.find("<div data-if=\"!selected.member_preview\">"));
  for (const char* event : {"copy_contact_id()", "block_contact()", "unblock_contact()", "remove_contact()"}) {
    EXPECT_NE(page.find(std::string("data-event-click=\"") + event + "\""), std::string::npos) << event;
  }
}

TEST(ContactCallLocaleTest, TheTwoNewLinesExistInBothLanguages) {
  for (const char* file : {"locales/en.json", "locales/zh-Hans.json"}) {
    const std::string json = ReadAsset(file);
    EXPECT_NE(json.find("\"contacts.call_not_ready\""), std::string::npos) << file;
    EXPECT_NE(json.find("\"contacts.call_blocked\""), std::string::npos) << file;
  }
}

// --- OnCallContact --------------------------------------------------------------------------------------------------

class ContactCallTest : public ::testing::Test {
protected:
  void SetUp() override {
    Add("c-ann", "account:ann", TrustLevel::Unknown);
    Add("c-bob", "account:bob", TrustLevel::Unknown);
    Add("c-eve", "account:eve", TrustLevel::Blocked);
    MessagingContactsPorts ports;
    ports.snapshot = []() {
      MessagingView view;
      view.initialized = true;
      return view;
    };
    ports.get_contact = [this](const std::string& id) -> Roe<std::optional<Contact>> {
      const auto it = contacts_.find(id);
      return it == contacts_.end() ? std::optional<Contact>{} : std::optional<Contact>{it->second};
    };
    ports.find_or_create_direct_thread = [this](const std::string& contact_id, ThreadChannel) -> Roe<Thread> {
      thread_for_.push_back(contact_id);
      Thread thread;
      thread.id = "thread-of-" + contact_id;
      return thread;
    };
    controller_.BindContactsPorts(ports);
    CallActionsPorts calls;
    calls.start_call = [this](const std::string& thread_id, const bool video) {
      started_.push_back({thread_id, video});
      return true;
    };
    calls.peer_calls_ready = [this]() { return ready_; };
    controller_.BindCallActions(calls);
    ShellFeedbackPorts feedback;
    feedback.show_toast = [this](const std::string& message, ToastDuration) { toasts_.push_back(message); };
    controller_.BindShellFeedback(feedback);
  }

  void Add(const std::string& id, const std::string& account, TrustLevel trust) {
    Contact contact;
    contact.id = id;
    contact.display_name = id;
    contact.remote.ids = {{mesh::ContactIdKind::Account, account, true}};
    contact.local.trust = trust;
    SyncContactMirrors(contact);
    contacts_[id] = contact;
  }

  std::map<std::string, Contact> contacts_;
  ContactsController controller_;
  std::vector<std::string> thread_for_;
  std::vector<std::pair<std::string, bool>> started_;
  std::vector<std::string> toasts_;
  bool ready_ = true;
};

TEST_F(ContactCallTest, VoiceAndVideoEachCallTheThreadOfThatContact) {
  controller_.OnCallContact("c-ann", false);
  controller_.OnCallContact("c-ann", true);
  ASSERT_EQ(started_.size(), 2u);
  EXPECT_EQ(started_[0], (std::pair<std::string, bool>{"thread-of-c-ann", false}));
  EXPECT_EQ(started_[1], (std::pair<std::string, bool>{"thread-of-c-ann", true}));
  EXPECT_EQ(thread_for_, (std::vector<std::string>{"c-ann", "c-ann"}));
  EXPECT_TRUE(toasts_.empty());
}

TEST_F(ContactCallTest, ThePageShowsBobButARowForAnnCallsAnn) {
  controller_.OnSelectContact("c-bob"); // the open page is Bob's
  controller_.OnCallContact("c-ann", true);
  ASSERT_EQ(started_.size(), 1u);
  EXPECT_EQ(started_[0].first, "thread-of-c-ann") << "the id passed in, not the selected contact";
  EXPECT_EQ(thread_for_, std::vector<std::string>{"c-ann"});
}

TEST_F(ContactCallTest, NotConnectedYetSaysSoAndCallsNobody) {
  ready_ = false;
  controller_.OnCallContact("c-ann", false);
  EXPECT_TRUE(started_.empty());
  EXPECT_TRUE(thread_for_.empty()) << "no chat is made for a call that is not placed";
  ASSERT_EQ(toasts_.size(), 1u);
  EXPECT_EQ(toasts_[0], Tr("contacts.call_not_ready"));
}

TEST_F(ContactCallTest, ABlockedContactIsNotCalled) {
  controller_.OnCallContact("c-eve", false);
  EXPECT_TRUE(started_.empty());
  ASSERT_EQ(toasts_.size(), 1u);
  EXPECT_EQ(toasts_[0], Tr("contacts.call_blocked"));
}

TEST_F(ContactCallTest, ACantMessageContactIsNotCalled) {
  Contact nobody;
  nobody.id = "c-none"; // no account id: nothing to reach
  contacts_["c-none"] = nobody;
  controller_.OnCallContact("c-none", false);
  EXPECT_TRUE(started_.empty());
  EXPECT_EQ(toasts_.size(), 1u) << "the page's own reason is shown";
}

TEST_F(ContactCallTest, AnUnknownContactDoesNothing) {
  controller_.OnCallContact("c-nobody", false);
  EXPECT_TRUE(started_.empty());
  EXPECT_TRUE(toasts_.empty());
}

// --- Block on the member page --------------------------------------------------------------------------------------

TEST_F(ContactCallTest, BlockingAStrangerAsksThenSavesThemAndTurnsThePageIntoTheirs) {
  std::vector<std::string> blocked;
  controller_.BindStrangerBlocker([this, &blocked](const std::string& account) -> Roe<std::string> {
    blocked.push_back(account);
    Add("c-new", account, TrustLevel::Blocked);
    return std::string("c-new");
  });
  std::string asked;
  ShellFeedbackPorts feedback;
  feedback.show_toast = [this](const std::string& message, ToastDuration) { toasts_.push_back(message); };
  feedback.show_confirm_destructive = [&asked](const std::string& title, const std::string&,
                                               std::function<void(bool)> on_result, const std::string&) {
    asked = title;
    on_result(true);
  };
  controller_.BindShellFeedback(feedback);

  controller_.OnShowMember("account:zed", "Zed", "");
  controller_.OnBlockMember();
  EXPECT_FALSE(asked.empty());
  EXPECT_EQ(blocked, std::vector<std::string>{"account:zed"});
  ASSERT_FALSE(toasts_.empty());
  EXPECT_EQ(toasts_.back(), Tr("contacts.toast.blocked", {{"name", "Zed"}}));
}

} // namespace
} // namespace pbr
