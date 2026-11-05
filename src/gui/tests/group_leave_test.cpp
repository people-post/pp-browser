// The owner leaving a group on a phone (DEV/PLAN-ui-batch8-group-pages.md 3): the sheet opens as a 4/5 page sheet,
// a pick asks once naming the person, Cancel does nothing, someone gone meanwhile is reported and nothing happens.
#include "foundation/i18n/LocalizationService.h"
#include "gui/chat/GroupLeaveSheet.h"
#include "lib/ui/ViewCatalog.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
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

class FakeHost : public GroupLeaveSheetHost {
public:
  OwnerLeaveSheet sheet;
  std::vector<PaneSpec> pushed;
  std::vector<int> open;
  std::vector<std::string> confirms;
  std::function<void(bool)> pending_confirm;
  bool still_possible = true;
  std::vector<std::string> handed_over;
  std::vector<std::string> dissolved;
  std::vector<std::string> toasts;

  OwnerLeaveSheet GroupLeaveRead(const std::string& thread_id) override {
    return sheet.thread_id == thread_id ? sheet : OwnerLeaveSheet{};
  }
  int GroupLeavePushSheet(const PaneSpec& spec) override {
    pushed.push_back(spec);
    open.push_back(static_cast<int>(pushed.size()));
    return open.back();
  }
  void GroupLeaveCloseSheet(const int id) override { std::erase(open, id); }
  bool GroupLeaveSheetOpen(const int id) const override {
    return std::find(open.begin(), open.end(), id) != open.end();
  }
  void GroupLeaveConfirm(const std::string& message, std::function<void(bool)> done) override {
    confirms.push_back(message);
    pending_confirm = std::move(done);
  }
  bool GroupLeaveStillPossible(const std::string&, const std::string&) override { return still_possible; }
  void GroupLeaveHandOver(const std::string& t, const std::string& g, const std::string& id) override {
    handed_over.push_back(t + ":" + g + ":" + id);
  }
  void GroupLeaveDissolve(const std::string& g) override { dissolved.push_back(g); }
  void GroupLeaveToast(const std::string& text) override { toasts.push_back(text); }
};

OwnerLeaveSheet Sheet(std::vector<OwnerLeaveCandidate> candidates) {
  OwnerLeaveSheet sheet;
  sheet.thread_id = "t1";
  sheet.group_id = "g1";
  sheet.candidates = std::move(candidates);
  return sheet;
}

class GroupLeaveTest : public ::testing::Test {
protected:
  static void SetUpTestSuite() { ASSERT_TRUE(LocalizationService::Instance().LoadFromAssets(PP_BROWSER_ASSETS_DIR)); }
  FakeHost host;
  GroupLeaveSheet sheet{host};
};

TEST_F(GroupLeaveTest, OpensAsALargePageSheetListingWhoCanTakeOver) {
  host.sheet = Sheet({{"acct:a", "Ann"}, {"acct:b", "Bea"}});
  ASSERT_TRUE(sheet.Open("t1"));
  ASSERT_EQ(host.pushed.size(), 1u);
  EXPECT_EQ(host.pushed[0].key, "group_leave");
  EXPECT_TRUE(host.pushed[0].page_sheet);
  EXPECT_EQ(OverlayEntry{}.sheet_detent, SheetDetent::Large);
  EXPECT_EQ(ViewCatalog::ResolvePath("group_leave"), "views/group_leave.rml");
  EXPECT_FALSE(sheet.View().empty);
  ASSERT_EQ(sheet.View().rows.size(), 2u);
  EXPECT_EQ(std::string(sheet.View().rows[0].name.c_str()), "Ann");
  EXPECT_TRUE(host.confirms.empty()) << "no confirm before a pick";
}

TEST_F(GroupLeaveTest, NotOpenedWhenTheOwnerIsNotLeavingThatChat) {
  host.sheet = Sheet({});
  EXPECT_FALSE(sheet.Open("other"));
  EXPECT_TRUE(host.pushed.empty());
}

TEST_F(GroupLeaveTest, APickAsksOnceNamingThePersonThenHandsOver) {
  host.sheet = Sheet({{"acct:a", "Ann"}});
  ASSERT_TRUE(sheet.Open("t1"));
  sheet.Pick("acct:a");
  ASSERT_EQ(host.confirms.size(), 1u);
  EXPECT_EQ(host.confirms[0], HandOverConfirmText("Ann"));
  EXPECT_NE(host.confirms[0].find("Ann"), std::string::npos);
  EXPECT_TRUE(host.handed_over.empty());
  host.pending_confirm(true);
  EXPECT_EQ(host.handed_over, (std::vector<std::string>{"t1:g1:acct:a"}));
  EXPECT_FALSE(sheet.IsOpen());
}

TEST_F(GroupLeaveTest, CancelDoesNothing) {
  host.sheet = Sheet({{"acct:a", "Ann"}});
  ASSERT_TRUE(sheet.Open("t1"));
  sheet.Pick("acct:a");
  host.pending_confirm(false);
  EXPECT_TRUE(host.handed_over.empty());
  EXPECT_TRUE(sheet.IsOpen());
}

TEST_F(GroupLeaveTest, SomeoneGoneMeanwhileIsReportedAndNothingHappens) {
  host.sheet = Sheet({{"acct:a", "Ann"}, {"acct:b", "Bea"}});
  ASSERT_TRUE(sheet.Open("t1"));
  sheet.Pick("acct:a");
  host.still_possible = false;
  host.sheet = Sheet({{"acct:b", "Bea"}}); // Ann left
  host.pending_confirm(true);
  EXPECT_TRUE(host.handed_over.empty());
  EXPECT_EQ(host.toasts, (std::vector<std::string>{HandOverGoneText("Ann")}));
  EXPECT_TRUE(sheet.IsOpen());
  ASSERT_EQ(sheet.View().rows.size(), 1u) << "the list is read again";
  EXPECT_EQ(std::string(sheet.View().rows[0].name.c_str()), "Bea");
}

TEST_F(GroupLeaveTest, NobodyToTakeOverShowsOnlyDissolve) {
  host.sheet = Sheet({});
  ASSERT_TRUE(sheet.Open("t1"));
  EXPECT_TRUE(sheet.View().empty);
  EXPECT_TRUE(sheet.View().rows.empty());
  sheet.Dissolve();
  EXPECT_EQ(host.dissolved, (std::vector<std::string>{"g1"}));
  const std::string rml = ReadAsset("views/group_leave.rml");
  EXPECT_NE(rml.find("data-if=\"empty\">{{i18n:group_leave.empty_title}}"), std::string::npos);
  EXPECT_NE(rml.find("class=\"cd-row cd-row--danger\""), std::string::npos);
  EXPECT_NE(rml.find("class=\"sheet-header\""), std::string::npos);
}

TEST_F(GroupLeaveTest, ClosesOnceTheGroupNoLongerNeedsIt) {
  host.sheet = Sheet({{"acct:a", "Ann"}});
  ASSERT_TRUE(sheet.Open("t1"));
  host.sheet = OwnerLeaveSheet{}; // dissolved meanwhile
  sheet.Tick(std::chrono::steady_clock::now() + std::chrono::seconds(2));
  EXPECT_FALSE(sheet.IsOpen());
  EXPECT_TRUE(host.open.empty());
}

} // namespace
} // namespace pbr
