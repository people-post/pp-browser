#pragma once

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#if defined(RMLUI_PLATFORM_EMSCRIPTEN) || defined(__ANDROID__) || (defined(__APPLE__) && TARGET_OS_IPHONE)
#define RMLUI_GL_ES3 1
#endif

#if defined(RMLUI_GL_ES3)
#define PP_BROWSER_SHADER_HEADER_GLES "#version 300 es\nprecision highp float;\n"
#define PP_BROWSER_SHADER_HEADER_GL "#version 330\n"
#else
#define PP_BROWSER_SHADER_HEADER_GL "#version 330\n"
#endif

inline bool UsesOpenGlEs() {
#if defined(RMLUI_GL_ES3)
  return true;
#else
  return false;
#endif
}
