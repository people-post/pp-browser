#pragma once

#include "foundation/platform/ui/DropCollector.h"
#include "lib/ui/ShellTypes.h"

namespace ui {
class ElementDocument;
}

namespace pbr {

/**
 * Is `point` (UI coordinates) inside the open chat's own pane in `document` (DEV/PLAN-desktop-drop-paste.md, C): the wide
 * layout's conversation column (`pane-chat`) or the narrow layout's chat page (`shell-chat-overlay`, absent while the
 * list shows). The left / top edge is inside, the right / bottom edge is not. False: no document, no such pane, or it
 * is not shown.
 */
bool DropPaneContains(ui::ElementDocument* document, LayoutMode layout_mode, DropUiPoint point);

} // namespace pbr
