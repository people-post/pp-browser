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
