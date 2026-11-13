#pragma once

#include "lib/ui/ShellTypes.h"

#include <ui/dom/Context.h>
#include <ui/dom/Element.h>
#include <ui/dom/ElementDocument.h>

#include <atomic>

namespace pbr {

/**
 * DEV/PLAN-call-frame-cost.md step 4: while the phone call page is full screen, the layers it covers are display:none
 * so the engine neither lays them out nor draws them (measured on an iPhone 11: UI thread -21..-23%). Whether they are
 * hidden is computed from the shell state on every sync (ShellHost::ApplySafeAreaLayout, after SyncLayout and every
 * call chrome remount), and they are shown at once whenever a call chrome snapshot says the page is no longer full
 * screen (ShellHost::ApplyCallChromeSnapshot), whichever path ended or shrank the call. Hiding unmounts nothing: a
 * draft, a scroll position and the pages stay as they were.
 *
 * The covered layers are the shell root's children under the call page (z-index 45): the base layer (tab / chat /
 * hidden-tab pages and the tab bar, up to 25), the AI drawer host (28) and the pushed-page layer (30). Everything above
 * the call page (overlays and dialogs 50, ring 53, PIN gate 55, photo 56, context menus 52, the startup cover 48) and
 * the emoji keyboard mount (a sibling of the shell root) stay.
 */
class ShellBaseHide {
public:
  static constexpr const char* kLayerIds[] = {"shell-base-layer", "shell-ai-drawer-host", "shell-transient-layer"};

  /** perf.threads: whether the layers were hidden when last applied. */
  static bool Hidden() { return HiddenFlag().load(std::memory_order_relaxed); }

  /**
   * Whether the layers under the call page are hidden for this shell state: a phone layout (the desktop and tablet
   * call bar covers nothing) and the call page full screen (the same test that mounts it: CallInProgressRml).
   */
  static bool ShouldHide(const ShellState& state) {
    return state.layout_mode == LayoutMode::Compact && state.call_in_progress.active &&
           state.call_in_progress.mode != CallChromeMode::Minimized;
  }

  /** Whether `element` is inside one of the layers while they are hidden (never focus such an element). */
  static bool InHiddenLayer(ui::ElementDocument* doc, ui::Element* element) {
    if (!Hidden() || !doc || !element) {
      return false;
    }
    for (const char* id : kLayerIds) {
      ui::Element* layer = doc->GetElementById(id);
      for (ui::Element* node = element; layer && node; node = node->GetParentNode()) {
        if (node == layer) {
          return true;
        }
      }
    }
    return false;
  }

  /** Sets (or clears) display:none on the covered layers of `doc`; the focus in a layer about to hide is dropped first. */
  static void Apply(ui::Context* context, ui::ElementDocument* doc, const bool hide) {
    if (hide && context) {
      HiddenFlag().store(true, std::memory_order_relaxed); // InHiddenLayer below asks about the layers being hidden
      if (ui::Element* focused = context->GetFocusElement(); focused && InHiddenLayer(doc, focused)) {
        focused->Blur(); // the keyboard goes with a text box's focus
        if (InHiddenLayer(doc, context->GetFocusElement())) {
          doc->Focus(); // Blur hands the focus to the parent, which is hidden too
        }
      }
    }
    HiddenFlag().store(hide, std::memory_order_relaxed);
    if (!doc) {
      return;
    }
    for (const char* id : kLayerIds) {
      ui::Element* layer = doc->GetElementById(id);
      if (!layer) {
        continue;
      }
      if (hide) {
        layer->SetProperty("display", "none");
      } else {
        layer->RemoveProperty("display");
      }
    }
  }

private:
  static std::atomic<bool>& HiddenFlag() {
    static std::atomic<bool> v{false};
    return v;
  }
};

} // namespace pbr
