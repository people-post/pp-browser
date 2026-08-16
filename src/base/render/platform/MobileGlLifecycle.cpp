#include "MobileGlLifecycle.h"

#include "GlBackend.h"

#include <cstdio>
#include <string>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#if TARGET_OS_IPHONE
#define GLES_SILENCE_DEPRECATION 1
#include <OpenGLES/ES3/gl.h>
#endif
#endif

#if SDL_MAJOR_VERSION >= 3
#include <SDL3/SDL.h>
#endif

namespace MobileGlLifecycle {

void ConfigureSdlGlAttributes() {
#if defined(RMLUI_GL_ES3)
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
#else
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
#endif
}

void SetMobileWindowCreateProperties(SDL_PropertiesID props) {
  // iOS: do not create as SDL fullscreen/borderless. UIKit still fills the
  // screen; those flags only hide the system status bar via
  // prefersStatusBarHidden. ShellHost already insets content for safe area.
  (void)props;
}

void InitIosDrawableFromWindow(SDL_Window* window, unsigned int& framebuffer, unsigned int& renderbuffer) {
#if defined(__APPLE__) && TARGET_OS_IPHONE
  const SDL_PropertiesID wprops = SDL_GetWindowProperties(window);
  framebuffer = static_cast<unsigned int>(
      SDL_GetNumberProperty(wprops, SDL_PROP_WINDOW_UIKIT_OPENGL_FRAMEBUFFER_NUMBER, 0));
  renderbuffer = static_cast<unsigned int>(
      SDL_GetNumberProperty(wprops, SDL_PROP_WINDOW_UIKIT_OPENGL_RENDERBUFFER_NUMBER, 0));
  if (framebuffer == 0) {
    GLint binding = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &binding);
    framebuffer = static_cast<unsigned int>(binding);
  }
  if (char* pref = SDL_GetPrefPath("dev.pp-browser", "pp-browser")) {
    const std::string path = std::string(pref) + "pp-browser-debug.log";
    SDL_free(pref);
    if (FILE* f = std::fopen(path.c_str(), "a")) {
      std::fprintf(f, "[I] iOS drawable FBO=%u RBO=%u\n", framebuffer, renderbuffer);
      std::fclose(f);
    }
  }
  std::fprintf(stderr, "[Frame] iOS drawable FBO=%u RBO=%u\n", framebuffer, renderbuffer);
#else
  (void)window;
  (void)framebuffer;
  (void)renderbuffer;
#endif
}

void UpdateIosDrawableFromWindow(SDL_Window* window, unsigned int& framebuffer, unsigned int& renderbuffer) {
#if defined(__APPLE__) && TARGET_OS_IPHONE
  const SDL_PropertiesID wprops = SDL_GetWindowProperties(window);
  framebuffer = static_cast<unsigned int>(
      SDL_GetNumberProperty(wprops, SDL_PROP_WINDOW_UIKIT_OPENGL_FRAMEBUFFER_NUMBER, 0));
  renderbuffer = static_cast<unsigned int>(
      SDL_GetNumberProperty(wprops, SDL_PROP_WINDOW_UIKIT_OPENGL_RENDERBUFFER_NUMBER, 0));
#else
  (void)window;
  (void)framebuffer;
  (void)renderbuffer;
#endif
}

void BindIosPresentTargets(unsigned int framebuffer, unsigned int renderbuffer) {
#if defined(__APPLE__) && TARGET_OS_IPHONE
  if (framebuffer != 0) {
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
  }
  if (renderbuffer != 0) {
    glBindRenderbuffer(GL_RENDERBUFFER, renderbuffer);
  }
#else
  (void)framebuffer;
  (void)renderbuffer;
#endif
}

} // namespace MobileGlLifecycle
