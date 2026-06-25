#pragma once

namespace Rml {
class Context;
}

#include <SDL3/SDL.h>

namespace pbr {

class SdlAppEvents {
public:
  static void Install();
  static bool PreProcess(Rml::Context* context, SDL_Event& event, bool& propagate_event);
};

} // namespace pbr
