#pragma once

#include "lib/ui/ShellTypes.h"

#include <ui/dom/Context.h>
#include <ui/dom/Element.h>
#include <ui/dom/ElementDocument.h>

#include <atomic>

namespace pbr {

/**
 * Measurement switch (DEV/PLAN-call-frame-cost.md step 0/4): with it on, the layers a full-screen phone call page
 * covers are display:none while it shows, to measure what the engine spends laying out and drawing them. Off by
 * default and not saved (a restart turns it off). The covered layers are the shell root's children under the call
 * page (z-index 45): the base layer (tab / chat / hidden-tab pages and the tab bar, up to 25), the AI drawer host (28)
 * and the pushed-page layer (30). Everything above the call page (overlays and dialogs 50, ring 53, PIN gate 55,
 * photo 56, context menus 52, the startup cover 48) and the emoji keyboard mount (a sibling of the shell root) stay.
 */
class ShellBaseHide {
public:
  static constexpr const char* kLayerIds[] = {"shell-base-layer", "shell-ai-drawer-host", "shell-transient-layer"};

  static bool SwitchOn() { return Switch().load(std::memory_order_relaxed); }
  static void SetSwitch(const bool on) { Switch().store(on, std::memory_order_relaxed); }
  /** perf.threads: whether the layers were hidden when last applied. */
  static bool Hidden() { return HiddenFlag().load(std::memory_order_relaxed); }

  /** Whether the layers under the call page are hidden for this shell state: the switch is on and the call page is full-screen. */
  static bool ShouldHide(const bool switch_on, const ShellState& state) {
    return switch_on && state.layout_mode == LayoutMode::Compact && state.call_in_progress.active &&
           state.call_in_progress.mode != CallChromeMode::Minimized;
  }

  /** Sets (or clears) display:none on the covered layers of `doc`; the focus in a layer about to hide is dropped first. */
  static void Apply(ui::Context* context, ui::ElementDocument* doc, const bool hide) {
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
        if (context) {
          if (ui::Element* focused = context->GetFocusElement()) {
            for (ui::Element* node = focused; node; node = node->GetParentNode()) {
              if (node == layer) {
                focused->Blur();
                break;
              }
            }
          }
        }
        layer->SetProperty("display", "none");
      } else {
        layer->RemoveProperty("display");
      }
    }
  }

private:
  static std::atomic<bool>& Switch() {
    static std::atomic<bool> v{false};
    return v;
  }
  static std::atomic<bool>& HiddenFlag() {
    static std::atomic<bool> v{false};
    return v;
  }
};

} // namespace pbr
