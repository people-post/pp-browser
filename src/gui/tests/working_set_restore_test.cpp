#include "gui/chat/WorkingSetController.h"

#include <gtest/gtest.h>

using namespace pbr;

namespace {

// Stands in for the expanded shell: the pane opens as soon as it becomes available.
struct FakeShell {
  bool available = false;
  bool open = false;

  ShellNavigationPorts Ports() {
    ShellNavigationPorts ports;
    ports.snapshot = [this] {
      ShellChromeSnapshot snap;
      snap.auxiliary_open = open;
      return snap;
    };
    ports.set_auxiliary_available = [this](const bool value) {
      if (value && !available) {
        open = true;
      }
      if (!value) {
        open = false;
      }
      available = value;
    };
    ports.open_auxiliary = [this] { open = available; };
    ports.close_auxiliary = [this] { open = false; };
    return ports;
  }
};

struct Fixture {
  bool active = false;
  ui::String title;
  ui::String subtitle;
  ui::String rml;
  TurnWidgetState widget;
  FakeShell shell;
  WorkingSetController controller{{active, title, subtitle, rml, widget}};

  Fixture() { controller.BindShellNavigation(shell.Ports()); }
};

std::vector<WorkingSetCandidate> OneCandidate() {
  WorkingSetCandidate candidate;
  candidate.block_index = 0;
  candidate.title = "Contacts";
  candidate.artifact_rml = "<p>x</p>";
  return {candidate};
}

} // namespace

TEST(WorkingSetRestoreTest, RestoreMakesThePaneAvailableWithoutOpeningIt) {
  Fixture f;
  (void)f.controller.RestoreEntry("entry-1", OneCandidate());
  EXPECT_TRUE(f.shell.available);
  EXPECT_FALSE(f.shell.open);
  EXPECT_TRUE(f.controller.HasEntry("entry-1"));
}

TEST(WorkingSetRestoreTest, RestoreLeavesAnAlreadyOpenPaneOpen) {
  Fixture f;
  f.shell.available = true;
  f.shell.open = true;
  (void)f.controller.RestoreEntry("entry-1", OneCandidate());
  EXPECT_TRUE(f.shell.open);
}

// The real sequence on a thread switch: ClearAll, then one RestoreEntry per stored message.
TEST(WorkingSetRestoreTest, ThreadSwitchRestoresSeveralEntriesWithoutOpening) {
  Fixture f;
  f.shell.available = true;
  f.shell.open = true;
  f.controller.ClearAll();
  EXPECT_FALSE(f.shell.open);
  (void)f.controller.RestoreEntry("entry-1", OneCandidate());
  (void)f.controller.RestoreEntry("entry-2", OneCandidate());
  EXPECT_TRUE(f.shell.available);
  EXPECT_FALSE(f.shell.open);
  EXPECT_TRUE(f.controller.HasEntry("entry-1"));
  EXPECT_TRUE(f.controller.HasEntry("entry-2"));
}

// B170: back in the chat by a way that cleared the panels without restoring them, a tapped list read
// "unavailable". Open() now reads the entry back from the thread.
TEST(WorkingSetRestoreTest, OpenReadsAClearedEntryBackFromTheThread) {
  Fixture f;
  (void)f.controller.RestoreEntry("entry-1", OneCandidate());
  f.controller.ClearAll();
  int loads = 0;
  f.controller.SetEntryLoader(
      [&loads](const std::string& entry_id) -> std::optional<WorkingSetController::StoredEntry> {
        ++loads;
        if (entry_id != "entry-1") {
          return std::nullopt;
        }
        return WorkingSetController::StoredEntry{OneCandidate(), {}};
      });
  f.controller.Open("entry-1", 0);
  EXPECT_EQ(loads, 1);
  EXPECT_TRUE(f.active);
  EXPECT_EQ(std::string(f.title.c_str()), "Contacts");
  EXPECT_EQ(std::string(f.rml.c_str()), "<p>x</p>");
  EXPECT_TRUE(f.shell.open);
  // Held again: a second open does not read the thread.
  f.controller.Open("entry-1", 0);
  EXPECT_EQ(loads, 1);
}

TEST(WorkingSetRestoreTest, OpenOfAnEntryNotInTheThreadStillSaysUnavailable) {
  Fixture f;
  f.controller.SetEntryLoader(
      [](const std::string&) -> std::optional<WorkingSetController::StoredEntry> { return std::nullopt; });
  f.controller.Open("gone", 0);
  EXPECT_TRUE(f.active);
  EXPECT_FALSE(f.controller.HasEntry("gone"));
  EXPECT_EQ(std::string(f.rml.c_str()).rfind("<p class=\"muted\">", 0), 0u); // the unavailable note
}
