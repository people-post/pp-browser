// "Send run logs" (DEV/PLAN-send-run-logs.md, step 1): words a person types must not reach the log. Each case puts a
// unique sentinel into one input path this target can drive, with every logger at the lowest level, and finds it in
// no line written. Here: a search typed on the Add contact page (its answers and its failure too). The PP 客服 and AI
// questions typed in the ask-AI panel are in ask_ai_panel_test.cpp and text pasted on the PIN gate in
// shell_host_test.cpp, next to the fakes they need.
#include "common/runtime/AppRuntime.h"
#include "foundation/i18n/LocalizationService.h"
#include "gui/contacts/ContactsController.h"
#include "gui/contacts/ContactsTickGate.h"
#include "gui/tests/run_log_sentinel_capture.h"

#include <gtest/gtest.h>

#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace pbr {
namespace {

using run_log_sentinel::kSentinel;

class RunLogSentinelSearchTest : public ::testing::Test {
protected:
  void SetUp() override {
    capture_ = &run_log_sentinel::Capture::Start();
    ContactsController::InstallInstance(controller_);
    AppRuntime::Initialize(AppRuntimeConfig{});
    ASSERT_TRUE(LocalizationService::Instance().LoadFromAssets(PP_BROWSER_ASSETS_DIR));
    LocalizationService::Instance().SetPreferredLanguage("en");
    ShellNavigationPorts nav;
    nav.push_layer = [](const PaneSpec&) { return 1; };
    controller_.BindShellNavigation(nav);
    MessagingContactsPorts ports;
    ports.search_people = [this](const std::string& query) -> Roe<std::vector<mesh::DirectoryHit>> {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        ++searches_;
      }
      if (fail_) {
        return Error(std::string("no answer for ") + query); // a server echo of the query, as some do
      }
      mesh::DirectoryHit hit;
      hit.display_name = std::string("Found ") + kSentinel;
      hit.nickname = std::string("found-") + kSentinel;
      return std::vector<mesh::DirectoryHit>{hit};
    };
    controller_.BindContactsPorts(ports);
    controller_.OpenAddContactPage();
    ASSERT_TRUE(controller_.AddContactPageOpen());
  }
  void TearDown() override {
    controller_.OnAddContactDismissed();
    controller_.BindContactsPorts({});
    controller_.BindShellNavigation({});
    AppRuntime::Shutdown();
    ContactsController::ClearInstance();
  }

  /** What the main loop does each frame, until the search ran and its answer is in (or 2 s went by). */
  void FramesUntilSearched() {
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < end) {
      if (ShouldTickContacts(NavTab::Sessions, controller_.DetailSheetOpen(), controller_.AddContactPageOpen())) {
        controller_.Tick();
      }
      AppRuntime::RunUITasks();
      if (!controller_.AddSearching()) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (searches_ > 0) {
          return;
        }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ADD_FAILURE() << "the search did not run";
  }

  run_log_sentinel::Capture* capture_ = nullptr;
  ContactsController controller_;
  std::mutex mutex_;
  int searches_ = 0;
  bool fail_ = false;
};

TEST_F(RunLogSentinelSearchTest, ASearchTermAndItsAnswersAreNeverLogged) {
  controller_.TypeInAddContact(std::string("find ") + kSentinel);
  FramesUntilSearched();
  capture_->ExpectNoSentinel();
}

TEST_F(RunLogSentinelSearchTest, ASearchTermIsNotLoggedWhenTheSearchFails) {
  fail_ = true;
  controller_.TypeInAddContact(std::string("find ") + kSentinel);
  FramesUntilSearched();
  capture_->ExpectNoSentinel();
}

} // namespace
} // namespace pbr
