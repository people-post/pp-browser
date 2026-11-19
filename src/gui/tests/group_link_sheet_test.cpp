// The invite link sheet holds the screen bright only while its layer is up, whichever way the layer goes, with no
// per-frame polling (the sheet has no tick).
#include "gui/QrBrightness.h"
#include "gui/chat/GroupLinkSheet.h"

#include "foundation/i18n/LocalizationService.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace pbr {
namespace {

class GroupLinkSheetTest : public ::testing::Test {
protected:
  static void SetUpTestSuite() { ASSERT_TRUE(LocalizationService::Instance().LoadFromAssets(PP_BROWSER_ASSETS_DIR)); }
  void SetUp() override {
    QrBrightness().Want(QrBrightnessHold::kGroupLink, false);
    QrBrightness().Want(QrBrightnessHold::kMyQr, false); // other fixtures leave their QR page open
    LocalizationService::Instance().SetPreferredLanguage("en");
    GroupLinkSheetPorts ports;
    ports.push_sheet = [this](const PaneSpec&) {
      open_.push_back(++last_id_);
      return last_id_;
    };
    ports.close_sheet = [this](const int id) { CloseLayer(id); };
    ports.watch_closed = [this](const int id, std::function<void()> closed) {
      watchers_.emplace(id, std::move(closed));
    };
    ports.load = [](const std::string&, std::function<void(Roe<GroupLinkRow>)> done) {
      GroupLinkRow row;
      row.link_id = "l1";
      row.text = "pp://join/abcdef";
      row.expires_at = 4102444800000;
      done(row);
    };
    ports.compact = []() { return true; };
    sheet_ = std::make_unique<GroupLinkSheet>(std::move(ports));
  }
  void TearDown() override {
    sheet_.reset();
    QrBrightness().Want(QrBrightnessHold::kGroupLink, false);
  }

  /** What ShellHost::CloseLayer does for any layer that goes: remove it, then tell the watchers (once). */
  void CloseLayer(const int id) {
    open_.erase(std::remove(open_.begin(), open_.end(), id), open_.end());
    auto range = watchers_.equal_range(id);
    std::vector<std::function<void()>> run;
    for (auto it = range.first; it != range.second; ++it) {
      run.push_back(std::move(it->second));
    }
    watchers_.erase(range.first, range.second);
    for (auto& closed : run) {
      closed();
    }
  }

  std::unique_ptr<GroupLinkSheet> sheet_;
  std::vector<int> open_;
  std::multimap<int, std::function<void()>> watchers_;
  int last_id_ = 20;
};

TEST_F(GroupLinkSheetTest, TheLinkOnScreenHoldsTheScreenBrightUntilItsLayerGoesByAnyPath) {
  sheet_->Open("g1");
  ASSERT_TRUE(sheet_->IsOpen());
  EXPECT_TRUE(QrBrightness().Raised());
  CloseLayer(21); // the scrim, a swipe, or a page cleared over it: nobody tells the sheet but the shell
  EXPECT_FALSE(sheet_->IsOpen());
  EXPECT_FALSE(QrBrightness().Raised());
  CloseLayer(21);
  EXPECT_FALSE(QrBrightness().Raised());
}

TEST_F(GroupLinkSheetTest, ClosingFromTheSheetRestoresOnce) {
  sheet_->Open("g1");
  ASSERT_TRUE(QrBrightness().Raised());
  sheet_->Close();
  EXPECT_FALSE(sheet_->IsOpen());
  EXPECT_FALSE(QrBrightness().Raised());
}

TEST_F(GroupLinkSheetTest, TheSheetOpenedAgainIsHeldByItsOwnLayer) {
  sheet_->Open("g1");
  CloseLayer(21);
  sheet_->Open("g1"); // layer 22
  ASSERT_TRUE(QrBrightness().Raised());
  EXPECT_EQ(watchers_.count(21), 0u);
  CloseLayer(22);
  EXPECT_FALSE(QrBrightness().Raised());
}

} // namespace
} // namespace pbr
