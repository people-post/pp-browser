#include "gui/shell/MascotFigure.h"

#include "gui/shell/MascotLook.h"
#include "lib/ui/CalendarHelper.h"

#include <string>

namespace pbr {

namespace {

// The layers of the AI dot's figure (assets/samples/window_shell.rml, #shell-ai-dot), in the order the shell draws them:
// part 0 the back bubble, 1 the front bubble, 2 the particles over both. ai_dot_model_test compares this list with the
// shell's markup look by look, so the two cannot drift apart.
constexpr MascotLayer kLayers[] = {
    {0, "", "body-back.svg", "abd"},
    {0, "", "body-back-c.svg", "c"},
    {0, "ai-dot-eyes-back ai-dot-open", "a-eyes-back.svg", "a"},
    {0, "ai-dot-eyes-back ai-dot-hap", "a-hap-back.svg", "a"},
    {0, "ai-dot-eyes-back ai-dot-shut", "a-shut-back.svg", "a"},
    {0, "ai-dot-eyes-back ai-dot-open", "b-eyes-back.svg", "b"},
    {0, "ai-dot-eyes-back ai-dot-hap", "b-hap-back.svg", "b"},
    {0, "ai-dot-eyes-back ai-dot-shut", "b-shut-back.svg", "b"},
    {0, "ai-dot-eyes-back ai-dot-open", "c-eyes-back.svg", "c"},
    {0, "ai-dot-eyes-back ai-dot-hap", "c-hap-back.svg", "c"},
    {0, "ai-dot-eyes-back ai-dot-shut", "c-shut-back.svg", "c"},
    {0, "ai-dot-arc ai-dot-arc-1", "arc-1.svg", "ab"},
    {0, "ai-dot-arc ai-dot-arc-2", "arc-2.svg", "ab"},
    {0, "ai-dot-arc ai-dot-arc-3", "arc-3.svg", "ab"},
    {0, "ai-dot-arc ai-dot-arc-1", "arc-c-1.svg", "c"},
    {0, "ai-dot-arc ai-dot-arc-2", "arc-c-2.svg", "c"},
    {0, "ai-dot-arc ai-dot-arc-3", "arc-c-3.svg", "c"},
    {1, "", "body-front.svg", "abd"},
    {1, "", "body-front-c.svg", "c"},
    {1, "ai-dot-blush", "a-blush.svg", "a"},
    {1, "ai-dot-blush", "b-blush.svg", "b"},
    {1, "ai-dot-eyes-front ai-dot-open", "a-eyes-front.svg", "a"},
    {1, "ai-dot-eyes-front ai-dot-hap", "a-hap-front.svg", "a"},
    {1, "ai-dot-eyes-front ai-dot-shut", "a-shut-front.svg", "a"},
    {1, "ai-dot-eyes-front ai-dot-open", "b-eyes-front.svg", "b"},
    {1, "ai-dot-eyes-front ai-dot-hap", "b-hap-front.svg", "b"},
    {1, "ai-dot-eyes-front ai-dot-shut", "b-shut-front.svg", "b"},
    {1, "ai-dot-eyes-front ai-dot-open", "c-eyes-front.svg", "c"},
    {1, "ai-dot-eyes-front ai-dot-hap", "c-hap-front.svg", "c"},
    {1, "ai-dot-eyes-front ai-dot-shut", "c-shut-front.svg", "c"},
    {1, "ai-dot-mouth", "a-mouth.svg", "a"},
    {1, "ai-dot-mouth", "b-mouth.svg", "b"},
    {1, "ai-dot-eq ai-dot-eq-1", "c-eq-1.svg", "c"},
    {1, "ai-dot-eq ai-dot-eq-2", "c-eq-2.svg", "c"},
    {1, "ai-dot-eq ai-dot-eq-3", "c-eq-3.svg", "c"},
    {1, "ai-dot-line ai-dot-line-1", "d-line-1.svg", "d"},
    {1, "ai-dot-line ai-dot-line-2", "d-line-2.svg", "d"},
    {1, "ai-dot-line ai-dot-line-3", "d-line-3.svg", "d"},
    {1, "ai-dot-bar ai-dot-bar-1", "d-bar-1.svg", "d"},
    {1, "ai-dot-bar ai-dot-bar-2", "d-bar-2.svg", "d"},
    {1, "ai-dot-bar ai-dot-bar-3", "d-bar-3.svg", "d"},
    {1, "ai-dot-bar ai-dot-bar-4", "d-bar-4.svg", "d"},
    {1, "ai-dot-bar ai-dot-bar-5", "d-bar-5.svg", "d"},
    {1, "ai-dot-ddot ai-dot-ddot-1", "d-dot-1.svg", "d"},
    {1, "ai-dot-ddot ai-dot-ddot-2", "d-dot-2.svg", "d"},
    {1, "ai-dot-ddot ai-dot-ddot-3", "d-dot-3.svg", "d"},
    {1, "ai-dot-check", "d-check.svg", "d"},
    {1, "ai-dot-dlock", "d-lock.svg", "d"},
    {2, "ai-dot-bubble", "bubble.svg", "ab"},
    {2, "ai-dot-bubble", "bubble-c.svg", "c"},
    {2, "ai-dot-tdot ai-dot-tdot-1", "dot-1.svg", "ab"},
    {2, "ai-dot-tdot ai-dot-tdot-2", "dot-2.svg", "ab"},
    {2, "ai-dot-tdot ai-dot-tdot-3", "dot-3.svg", "ab"},
    {2, "ai-dot-tdot ai-dot-tdot-1", "dot-c-1.svg", "c"},
    {2, "ai-dot-tdot ai-dot-tdot-2", "dot-c-2.svg", "c"},
    {2, "ai-dot-tdot ai-dot-tdot-3", "dot-c-3.svg", "c"},
    {2, "ai-dot-star", "star.svg", "ab"},
    {2, "ai-dot-spark", "spark-c.svg", "c"},
    {2, "ai-dot-spark", "spark-d.svg", "d"},
    {2, "ai-dot-heart ai-dot-heart-1", "heart-1.svg", "ab"},
    {2, "ai-dot-heart ai-dot-heart-2", "heart-2.svg", "ab"},
    {2, "ai-dot-z ai-dot-z-1", "z-1.svg", "ab"},
    {2, "ai-dot-z ai-dot-z-2", "z-2.svg", "ab"},
    {2, "ai-dot-z ai-dot-z-3", "z-3.svg", "ab"},
    {2, "ai-dot-lock", "lock.svg", "ab"},
    {2, "ai-dot-lock", "lock-c.svg", "c"},};

} // namespace

std::span<const MascotLayer> MascotLayers() {
  return kLayers;
}

std::string MascotFigureRml(const char parts) {
  std::string part_rml[3];
  for (const MascotLayer& layer : kLayers) {
    bool used = false;
    for (const char* look = layer.looks; *look != '\0'; ++look) {
      used = used || *look == parts;
    }
    if (!used) {
      continue;
    }
    std::string& out = part_rml[layer.part];
    out += "<svg class=\"ai-dot-layer";
    if (layer.classes[0] != '\0') {
      out += ' ';
      out += layer.classes;
    }
    out += "\" src=\"../icons/mascot/";
    out += layer.src;
    out += "\"></svg>";
  }
  return "<div class=\"ai-dot-figure\"><div class=\"ai-dot-part ai-dot-back\">" + part_rml[0] +
         "</div><div class=\"ai-dot-part ai-dot-front\">" + part_rml[1] + "</div>" + part_rml[2] + "</div>";
}

char ResolveLookToday(const std::string_view pref) {
  const TodayYmd today = TodayLocalDate();
  return ResolveLook(pref, today.year, today.month, today.day);
}

} // namespace pbr
