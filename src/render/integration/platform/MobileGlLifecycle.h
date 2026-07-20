#pragma once

#include <SDL3/SDL.h>

namespace MobileGlLifecycle {

void ConfigureSdlGlAttributes();
void SetMobileWindowCreateProperties(SDL_PropertiesID props);
void InitIosDrawableFromWindow(SDL_Window* window, unsigned int& framebuffer, unsigned int& renderbuffer);
void UpdateIosDrawableFromWindow(SDL_Window* window, unsigned int& framebuffer, unsigned int& renderbuffer);
void BindIosPresentTargets(unsigned int framebuffer, unsigned int renderbuffer);

} // namespace MobileGlLifecycle
