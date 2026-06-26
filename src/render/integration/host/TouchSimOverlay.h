#pragma once

union SDL_Event;
struct SDL_Window;

namespace TouchSimOverlay {

void Initialize(SDL_Window* window);
void Shutdown();
void Draw(SDL_Window* window, int viewport_w, int viewport_h);

} // namespace TouchSimOverlay
