#include "gui/chat/VoiceBubble.h"

#include "foundation/i18n/LocalizationService.h"
#include "lib/ai/StructuredTextParser.h"

#include <algorithm>
#include <cctype>
#include <cstdint>

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

double SeekFractionAt(const float x, const float left, const float width) {
  if (width <= 0.f) {
    return 0.0;
  }
  return std::clamp(static_cast<double>(x - left) / static_cast<double>(width), 0.0, 1.0);
}

int VoiceBubbleFillPercent(const VoiceBubbleState& state) {
  if ((!state.playing && !state.paused) || state.duration_ms <= 0) {
    return 0;
  }
  return static_cast<int>(std::clamp<int64_t>(state.position_ms * 100 / state.duration_ms, 0, 100));
}

std::vector<int> VoiceWaveformHeights(const std::string& message_id, const int count) {
  // FNV-1a of the id seeds a small xorshift; the same id always gives the same bars (stable across redraws).
  uint64_t h = 1469598103934665603ull;
  for (const char c : message_id) {
    h = (h ^ static_cast<unsigned char>(c)) * 1099511628211ull;
  }
  uint64_t x = h ? h : 0x9e3779b97f4a7c15ull;
  auto next = [&x]() {
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    return static_cast<double>(x >> 11) / static_cast<double>(1ull << 53); // 0..1
  };
  std::vector<int> bars;
  bars.reserve(static_cast<size_t>(std::max(count, 0)));
  double level = 0.3 + 0.4 * next();
  for (int i = 0; i < count; ++i) {
    // A walk with big enough steps to look like speech: loud and quiet stretches, never flat.
    level = std::clamp(level + (next() - 0.5) * 0.9, 0.08, 1.0);
    const double mix = 0.35 * level + 0.65 * next() * level + 0.1;
    bars.push_back(4 + static_cast<int>(std::clamp(mix, 0.0, 1.0) * 24.0 + 0.5));
  }
  return bars;
}

std::string VoiceBubbleRml(const std::string& message_id, const std::string& click_arg, const int64_t duration_ms,
                           const VoiceBubbleState& state, const VoiceBubbleFace& face) {
  const std::string key = VoiceElementKey(message_id);
  const bool running = state.playing && !state.paused;
  const int percent = VoiceBubbleFillPercent(state);
  std::string bars;
  for (const int height : VoiceWaveformHeights(message_id)) {
    bars += "<div class=\"chat-voice-bar\" style=\"height: " + std::to_string(height) + "dp;\"></div>";
  }
  std::string html = std::string("<div class=\"chat-card chat-voice-card") +
                     (face.ours ? " chat-voice-card--out" : "") + "\"><div class=\"chat-voice-main\">";
  html += "<div class=\"chat-voice-avatar avatar-tone-" + std::to_string(std::clamp(face.tone, 0, 7)) + "\">";
  if (!face.icon_path.empty()) {
    html += "<img class=\"avatar-photo chat-voice-avatar-photo\" src=\"" +
            StructuredTextParser::EscapeText(face.icon_path) + "\"/>";
  } else {
    html += "<p class=\"chat-voice-avatar-letter\">" + StructuredTextParser::EscapeText(face.letter) + "</p>";
  }
  html +=
      "</div><div class=\"chat-voice-badge\"><svg class=\"chat-voice-badge-mic\" src=\"../icons/v2/mic.svg\" "
      "width=\"10\" height=\"10\"></svg></div>";
  html += "<button type=\"button\" id=\"vp-" + key + "\" class=\"chat-voice-play" +
          std::string(running ? " chat-voice-play--playing" : "") + "\" title=\"" +
          StructuredTextParser::EscapeText(Tr(running ? "chat.voice.pause" : "chat.voice.play")) +
          "\" data-event-click=\"play_voice('" + click_arg + "')\">";
  html +=
      "<svg class=\"chat-voice-icon-play\" src=\"../icons/play.svg\" width=\"22\" height=\"22\" "
      "crop-to-content=\"true\"></svg>";
  html +=
      "<svg class=\"chat-voice-icon-pause\" src=\"../icons/pause.svg\" width=\"22\" height=\"22\" "
      "crop-to-content=\"true\"></svg>";
  html += "</button>";
  // The waveform: the quiet bars, over them a clip as wide as the played part holding the same bars in the played
  // colour, and the knob at its end (VoicePlayback sets both by percent). Seeking by tap is not offered: a voice
  // message is played by the clip player, which cannot seek.
  html += "<div class=\"chat-voice-wave\"><div class=\"chat-voice-bars\">" + bars + "</div>";
  html += "<div id=\"vf-" + key + "\" class=\"chat-voice-played\" style=\"width: " + std::to_string(percent) +
          "%;\"><div class=\"chat-voice-bars chat-voice-bars--played\">" + bars + "</div></div>";
  html += "<div id=\"vn-" + key + "\" class=\"chat-voice-knob\" style=\"left: " + std::to_string(percent) +
          "%;\"></div></div></div>";
  html += "<div class=\"chat-voice-foot\"><span id=\"vt-" + key + "\" class=\"chat-voice-time\">" +
          StructuredTextParser::EscapeText(VoiceBubbleTimeText(state, duration_ms)) + "</span>";
  if (!face.clock.empty()) {
    html += "<span class=\"chat-voice-clock\">" + StructuredTextParser::EscapeText(face.clock) + "</span>";
  }
  html += "</div></div>";
  return html;
}

std::string AudioFileBubbleRml(const std::string& message_id, const std::string& click_arg, const AudioCardFace& face,
                               const VoiceBubbleState& state) {
  const std::string key = VoiceElementKey(message_id);
  const bool running = state.playing && !state.paused;
  const bool started = state.playing || state.paused;
  std::string html = "<div class=\"chat-card chat-attachment-card chat-audio-card\">";
  html += "<div class=\"chat-audio-head\"><div class=\"chat-audio-cover\">";
  if (!face.cover_path.empty()) {
    html += "<img class=\"chat-audio-cover-image\" src=\"" + StructuredTextParser::EscapeText(face.cover_path) + "\"/>";
  } else {
    html += kAudioCoverNoteRml;
  }
  html += "</div><div class=\"chat-audio-meta\">";
  html += "<p class=\"text chat-audio-title\">" + StructuredTextParser::EscapeText(face.title) + "</p>";
  if (!face.subtitle.empty()) {
    html += "<p class=\"muted chat-audio-sub\">" + StructuredTextParser::EscapeText(face.subtitle) + "</p>";
  }
  html += "</div></div>";
  html += "<div class=\"chat-audio-controls\">";
  html += "<button type=\"button\" id=\"vp-" + key + "\" class=\"chat-voice-play" +
          std::string(running ? " chat-voice-play--playing" : "") + "\" title=\"" +
          StructuredTextParser::EscapeText(Tr(running ? "chat.voice.pause" : "chat.voice.play")) +
          "\" data-event-click=\"play_audio_file('" + click_arg + "')\">";
  html +=
      "<svg class=\"chat-voice-icon-play\" src=\"../icons/play.svg\" width=\"16\" height=\"16\" "
      "crop-to-content=\"true\"></svg>";
  html +=
      "<svg class=\"chat-voice-icon-pause\" src=\"../icons/pause.svg\" width=\"16\" height=\"16\" "
      "crop-to-content=\"true\"></svg>";
  html += "</button>";
  // The bar is 4dp thin: its wrapper is the 32dp hit area of a tap or drag (audio_seek_press follows the drag).
  html += "<div id=\"vk-" + key + "\" class=\"chat-audio-seek\" data-event-mousedown=\"audio_seek_press()\">";
  html += "<div class=\"chat-voice-track\"><div id=\"vf-" + key +
          "\" class=\"chat-voice-fill\" style=\"width: " + std::to_string(VoiceBubbleFillPercent(state)) +
          "%;\"></div></div>";
  // The drag knob rides the end of the filled part (white on an own bubble, dark on a received one: see the theme);
  // it sits outside the track (which clips) and lets the press through to the wrapper.
  html += "<div id=\"vn-" + key +
          "\" class=\"chat-audio-knob\" style=\"left: " + std::to_string(VoiceBubbleFillPercent(state)) +
          "%;\"></div></div>";
  const std::string time = started ? VoiceBubbleTimeText(state, face.duration_ms)
                                   : (face.duration_ms > 0 ? FormatVoiceDuration(face.duration_ms) : std::string());
  html += "<span id=\"vt-" + key + "\" class=\"chat-voice-time chat-audio-time\">" +
          StructuredTextParser::EscapeText(time) + "</span>";
  html += "</div></div>";
  return html;
}

} // namespace pbr
