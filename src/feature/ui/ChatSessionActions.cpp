#include "feature/ui/ChatSessionActions.h"

namespace pbr {

ChatSessionActions& ChatSessionActions::Instance() {
  static ChatSessionActions actions;
  return actions;
}

} // namespace pbr
