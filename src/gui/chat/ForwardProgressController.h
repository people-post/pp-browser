#pragma once

#include "gui/FlowCoordinatorPorts.h"
#include "gui/chat/ForwardFlow.h"
#include "gui/shell/ShellNavigationPorts.h"

#include <ui/base/Types.h>

#include <functional>

namespace ui {
class Context;
}

namespace pbr {

/**
 * The "Forwarding 2/7" page (DEV/PLAN-message-forward-files-v3.md, design 3): a full-screen layer over the chat while a
 * forward with attachments is sent, with the progress, a Cancel and the line "stay on this page". It has no edge-swipe
 * back (the shell lists none for its key); the back key and Esc are Cancel (the flow coordinator asks `on_back`), and
 * a layer closed by someone else (a tapped notification clears them all) tells `on_closed`. Pure display: every decision
 * is the flow's (ForwardFlow). UI thread.
 */
class ForwardProgressController {
public:
  struct Handlers {
    std::function<void()> cancel;
    std::function<void()> close;
    /** The back key / Esc: true when it was taken (the page stays up). */
    std::function<bool()> back;
    /** The layer was closed by someone else. */
    std::function<void()> closed;
  };

  void BindShellNavigation(ShellNavigationPorts ports) { navigation_ = std::move(ports); }
  void BindFlowCoordinator(FlowCoordinatorPorts ports) { flow_ = std::move(ports); }
  void SetHandlers(Handlers handlers) { handlers_ = std::move(handlers); }
  bool RegisterModel(ui::Context* context);

  /** Opens the layer; does nothing when it is open or the shell cannot show a layer. */
  void Open();
  void Close();
  bool IsOpen() const { return layer_id_ >= 0; }
  void Show(const ForwardProgressView& view);

private:
  ShellNavigationPorts navigation_;
  FlowCoordinatorPorts flow_;
  Handlers handlers_;
  ui::Context* context_ = nullptr;
  int layer_id_ = -1;
  ui::String title_;
  ui::String line_;
  ui::String ring_decor_ = "none";
  ui::String percent_text_ = "0%";
  bool has_percent_ = false;
  bool finishing_ = false;
  bool can_close_ = false;
};

} // namespace pbr
