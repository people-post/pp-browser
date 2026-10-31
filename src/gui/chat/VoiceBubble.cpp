#include "gui/chat/VoiceBubble.h"

#include "foundation/i18n/LocalizationService.h"
#include "lib/ai/StructuredTextParser.h"

#include <algorithm>
#include <cctype>

namespace pbr {

std::string VoiceElementKey(const std::string& message_id) {
  std::string key;
  key.reserve(message_id.size());
  for (const char c : message_id) {
    key.push_back(std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' ? c : '_');
  }
  return key;
}

std::string VoiceBubbleTimeText(const VoiceBubbleState& state, const int64_t named_duration_ms) {
  if (!state.playing && !state.paused) {
    return FormatVoiceDuration(named_duration_ms);
  }
  const int64_t total = state.duration_ms > 0 ? state.duration_ms : named_duration_ms;
  // Position counts up from 0:00 (not rounded up like a length).
  const int64_t seconds = std::max<int64_t>(0, state.position_ms) / 1000;
  const std::string position =
      std::to_string(seconds / 60) + ":" + (seconds % 60 < 10 ? "0" : "") + std::to_string(seconds % 60);
  return position + " / " + FormatVoiceDuration(total);
}

int VoiceBubbleFillPercent(const VoiceBubbleState& state) {
  if ((!state.playing && !state.paused) || state.duration_ms <= 0) {
    return 0;
  }
  return static_cast<int>(std::clamp<int64_t>(state.position_ms * 100 / state.duration_ms, 0, 100));
}

std::string VoiceBubbleRml(const std::string& message_id, const std::string& click_arg, const int64_t duration_ms,
                           const VoiceBubbleState& state) {
  const std::string key = VoiceElementKey(message_id);
  const bool running = state.playing && !state.paused;
  std::string html = "<div class=\"chat-card chat-voice-card\">";
  html += "<button type=\"button\" id=\"vp-" + key + "\" class=\"chat-voice-play" +
          std::string(running ? " chat-voice-play--playing" : "") + "\" title=\"" +
          StructuredTextParser::EscapeText(Tr(running ? "chat.voice.pause" : "chat.voice.play")) +
          "\" data-event-click=\"play_voice('" + click_arg + "')\">";
  html += "<svg class=\"chat-voice-icon-play\" src=\"../icons/play.svg\" width=\"16\" height=\"16\" crop-to-content=\"true\"></svg>";
  html += "<svg class=\"chat-voice-icon-pause\" src=\"../icons/pause.svg\" width=\"16\" height=\"16\" crop-to-content=\"true\"></svg>";
  html += "</button>";
  html += "<div class=\"chat-voice-track\"><div id=\"vf-" + key + "\" class=\"chat-voice-fill\" style=\"width: " +
          std::to_string(VoiceBubbleFillPercent(state)) + "%;\"></div></div>";
  html += "<span id=\"vt-" + key + "\" class=\"chat-voice-time\">" +
          StructuredTextParser::EscapeText(VoiceBubbleTimeText(state, duration_ms)) + "</span>";
  html += "</div>";
  return html;
}

} // namespace pbr
