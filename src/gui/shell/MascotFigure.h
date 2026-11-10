#pragma once

#include <span>
#include <string>
#include <string_view>

namespace pbr {

/** One SVG layer of the mascot's figure and the looks (letters `a`–`g`) it is drawn in. */
struct MascotLayer {
  int part;            ///< 0 back bubble, 1 front bubble, 2 over both.
  const char* classes; ///< After `ai-dot-layer`.
  const char* src;     ///< File in assets/icons/mascot/.
  const char* looks;
};

std::span<const MascotLayer> MascotLayers();

/**
 * The figure of the AI dot for the artwork `parts` (a letter of MascotLayers; PartsFor maps a day's look to one), as
 * RML for `ai-dot-figure`'s place inside an element with class `ai-dot`: the same layers and classes as the shell's dot,
 * so the dot's keyframes (`ai-dot--<action>` on that element) move it. Used by the AI Home's mascot.
 */
std::string MascotFigureRml(char parts);

/** ResolveLook (MascotLook.h) for `pref` on the local date today. */
char ResolveLookToday(std::string_view pref);

} // namespace pbr
