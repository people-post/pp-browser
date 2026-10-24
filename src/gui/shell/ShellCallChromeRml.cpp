#include "gui/shell/ShellCallChromeRml.h"

#include "foundation/i18n/LocalizationService.h"

#include <sstream>

namespace pbr {

std::string CallRingRml(const ShellState& state) {
  // Mounted into #shell-call-ring-mount when active (RemountCallChrome). Presence = visibility;
  // data-if alone was not flipping Display on idle Present (accept dialog stayed gone).
  if (!state.call_ring.active) {
    return {};
  }
  std::ostringstream out;
  out << "<div class=\"shell-layer shell-layer-dialog\" data-model=\"window\">";
  out << "<div class=\"shell-scrim\"></div>";
  out << "<div class=\"shell-dialog shell-call-ring\" data-class-shell-call-ring--pulse=\"call_ring_pulse\">";
  out << "<p class=\"text-sm shell-call-ring-eyebrow\">{{call_ring_eyebrow}}</p>";
  out << "<h2 class=\"heading-2 shell-dialog-title\">{{call_ring_media}}</h2>";
  out << "<p class=\"text shell-dialog-message\" data-if=\"!call_ring_conflict\">{{call_ring_caller}}</p>";
  out << "<p class=\"text shell-dialog-message\" data-if=\"call_ring_conflict\">{{call_ring_conflict_hint}}</p>";
  out << "<p class=\"text-sm shell-dialog-message\" data-if=\"call_ring_show_pricing\" "
         ">{{call_ring_pricing_label}}</p>";
  // WeChat-style round icon buttons with a short label underneath; long labels above
  // (call_ring_decline_label / accept_label / voice_answer_label) stay bound for a11y.
  out << "<div class=\"shell-call-ring-actions row\">";
  out << "<div class=\"shell-call-ring-action\">";
  out << "<button class=\"shell-call-ring-action-circle shell-call-ring-decline\" type=\"button\" "
         "data-event-click=\"call_decline()\">";
  out << "<svg src=\"../icons/phone-hangup.svg\" width=\"26\" height=\"26\" crop-to-content=\"true\"></svg>";
  out << "</button>";
  out << "<p class=\"text-xs shell-call-ring-action-label\">" << Tr("call.ring.short.decline") << "</p>";
  out << "</div>";
  out << "<div class=\"shell-call-ring-action\" data-if=\"call_ring_video_allowed\">";
  out << "<button class=\"shell-call-ring-action-circle shell-call-ring-voice\" type=\"button\" "
         "data-event-click=\"call_accept_voice()\">";
  out << "<svg src=\"../icons/phone.svg\" width=\"26\" height=\"26\" crop-to-content=\"true\"></svg>";
  out << "</button>";
  out << "<p class=\"text-xs shell-call-ring-action-label\">{{call_ring_voice_short}}</p>";
  out << "</div>";
  out << "<div class=\"shell-call-ring-action\">";
  out << "<button class=\"shell-call-ring-action-circle shell-call-accept\" "
         "data-class-shell-call-accept--pulse=\"call_ring_pulse\" type=\"button\" "
         "data-event-click=\"call_accept()\">";
  out << "<svg width=\"26\" height=\"26\" crop-to-content=\"true\" "
         "data-attr-src=\"call_ring_video_allowed ? '../icons/video.svg' : '../icons/phone.svg'\"></svg>";
  out << "</button>";
  out << "<p class=\"text-xs shell-call-ring-action-label\">{{call_ring_accept_short}}</p>";
  out << "</div>";
  out << "</div>";
  out << "<div class=\"shell-dialog-actions column\" data-if=\"call_ring_show_pricing\">";
  out << "<button class=\"btn btn-secondary\" type=\"button\" "
         "data-attrif-disabled=\"!call_ring_accept_charge_enabled\" "
         "data-event-click=\"call_accept_charge()\">{{call_ring_accept_charge_label}}</button>";
  out << "<p class=\"text-xs shell-dialog-message\" data-if=\"!call_ring_accept_charge_enabled\" "
         ">{{call_ring_accept_charge_hint}}</p>";
  out << "</div></div></div>";
  return out.str();
}

std::string CallInProgressRml(const ShellState& state) {
  // V031 modes: Expanded (top bar), Immersive (people grid), Minimized (corner chip).
  // Mounted into #shell-call-in-progress-mount when active; video tiles still DirtyCallChrome-only.
  if (!state.call_in_progress.active) {
    return {};
  }
  auto append_quality_chip = [](std::ostringstream& out, const char* extra_class) {
    out << "<button class=\"shell-call-quality " << extra_class
        << "\" type=\"button\" data-event-click=\"call_details()\" "
           "data-class-shell-call-quality--ok=\"call_in_progress_quality_ok\" "
           "data-class-shell-call-quality--warn=\"call_in_progress_quality_warn\" "
           "data-class-shell-call-quality--error=\"call_in_progress_quality_error\">";
    out << "<div class=\"shell-call-quality-bars row\">";
    out << "<div class=\"shell-call-quality-bar shell-call-quality-bar--1\" "
           "data-class-shell-call-quality-bar--on=\"call_in_progress_quality_bars >= 1\"></div>";
    out << "<div class=\"shell-call-quality-bar shell-call-quality-bar--2\" "
           "data-class-shell-call-quality-bar--on=\"call_in_progress_quality_bars >= 2\"></div>";
    out << "<div class=\"shell-call-quality-bar shell-call-quality-bar--3\" "
           "data-class-shell-call-quality-bar--on=\"call_in_progress_quality_bars >= 3\"></div>";
    out << "<div class=\"shell-call-quality-bar shell-call-quality-bar--4\" "
           "data-class-shell-call-quality-bar--on=\"call_in_progress_quality_bars >= 4\"></div>";
    out << "</div>";
    out << "<span class=\"text-xs shell-call-quality-label\" data-if=\"call_in_progress_quality_label != ''\" "
           ">{{call_in_progress_quality_label}}</span>";
    out << "</button>";
  };

  auto append_core_actions = [](std::ostringstream& out) {
    out << "<button class=\"shell-call-retry\" type=\"button\" data-if=\"call_in_progress_show_retry\" "
           "data-event-click=\"call_retry()\">";
    out << "<svg src=\"../icons/sync.svg\" width=\"18\" height=\"18\" crop-to-content=\"true\"></svg>";
    out << "</button>";
    out << "<button class=\"shell-call-invite\" type=\"button\" data-if=\"call_in_progress_show_invite\" "
           "data-event-click=\"call_invite()\">";
    out << "<svg src=\"../icons/plus.svg\" width=\"18\" height=\"18\" crop-to-content=\"true\"></svg>";
    out << "</button>";
    out << "<button id=\"shell-call-mute-btn\" class=\"shell-call-mute\" type=\"button\" "
           "data-class-shell-call-mute--on=\"call_in_progress_muted\" data-event-click=\"call_mute()\">";
    out << "<svg id=\"shell-call-mute-icon\" width=\"18\" height=\"18\" crop-to-content=\"true\" "
           "data-attr-src=\"call_in_progress_muted ? '../icons/mic-off.svg' : '../icons/mic.svg'\"></svg>";
    out << "</button>";
    out << "<button id=\"shell-call-speaker-btn\" class=\"shell-call-speaker\" type=\"button\" "
           "data-if=\"call_in_progress_show_speaker\" "
           "data-class-shell-call-speaker--on=\"call_in_progress_speaker_on\" data-event-click=\"call_speaker()\">";
    out << "<svg id=\"shell-call-speaker-icon\" src=\"../icons/speaker.svg\" width=\"18\" height=\"18\" "
           "crop-to-content=\"true\"></svg>";
    out << "</button>";
    out << "<button class=\"shell-call-camera\" type=\"button\" data-if=\"call_in_progress_show_camera\" "
           "data-class-shell-call-camera--on=\"call_in_progress_camera_on\" data-event-click=\"call_camera()\">";
    out << "<svg width=\"18\" height=\"18\" crop-to-content=\"true\" "
           "data-attr-src=\"call_in_progress_camera_on ? '../icons/video.svg' : '../icons/video-off.svg'\"></svg>";
    out << "</button>";
    out << "<button class=\"shell-call-leave\" type=\"button\" data-event-click=\"call_leave()\">";
    out << "<svg src=\"../icons/phone-hangup.svg\" width=\"18\" height=\"18\" crop-to-content=\"true\"></svg>";
    out << "</button>";
  };

  const auto& call = state.call_in_progress;
  std::ostringstream out;

  if (call.mode == CallChromeMode::Minimized) {
    const char* corner_class = "shell-call-minimized-chip--tr";
    switch (call.minimized_corner) {
    case 1:
      corner_class = "shell-call-minimized-chip--tl";
      break;
    case 2:
      corner_class = "shell-call-minimized-chip--br";
      break;
    case 3:
      corner_class = "shell-call-minimized-chip--bl";
      break;
    default:
      break;
    }
    out << "<div id=\"shell-call-chrome-root\" class=\"shell-layer shell-layer-call-minimized\" "
           "data-model=\"window\">";
    out << "<div id=\"shell-call-minimized-chip\" class=\"shell-call-minimized-chip row " << corner_class
        << "\">";
    out << "<div class=\"shell-call-minimized-main\">";
    out << "<p class=\"text-sm shell-call-minimized-title\">{{call_in_progress_title}}</p>";
    out << "<p class=\"text-xs shell-call-minimized-subtitle\">{{call_in_progress_subtitle}}</p>";
    out << "</div>";
    append_quality_chip(out, "shell-call-quality--compact");
    out << "<button id=\"shell-call-mute-btn\" class=\"shell-call-mute shell-call-mute--compact\" type=\"button\" "
           "data-class-shell-call-mute--on=\"call_in_progress_muted\" data-event-click=\"call_mute()\">";
    out << "<svg id=\"shell-call-mute-icon\" width=\"16\" height=\"16\" crop-to-content=\"true\" "
           "data-attr-src=\"call_in_progress_muted ? '../icons/mic-off.svg' : '../icons/mic.svg'\"></svg>";
    out << "</button>";
    out << "<button class=\"shell-call-leave shell-call-leave--compact\" type=\"button\" "
           "data-event-click=\"call_leave()\">";
    out << "<svg src=\"../icons/phone-hangup.svg\" width=\"16\" height=\"16\" crop-to-content=\"true\"></svg>";
    out << "</button>";
    out << "</div></div>";
    return out.str();
  }

  if (call.mode == CallChromeMode::Immersive) {
    out << "<div id=\"shell-call-chrome-root\" class=\"shell-layer shell-layer-call-immersive\" "
           "data-model=\"window\">";
    out << "<div class=\"shell-call-immersive\">";
    out << "<div class=\"shell-call-immersive-header\">";
    out << "<div class=\"shell-call-immersive-grabber\"></div>";
    out << "<div class=\"shell-call-bar-row row\">";
    out << "<div class=\"shell-call-bar-main\">";
    out << "<p class=\"text-sm shell-call-bar-title\">{{call_in_progress_title}}</p>";
    out << "<p class=\"text-sm shell-call-bar-subtitle\">{{call_in_progress_subtitle}}</p>";
    out << "<p class=\"text-xs shell-call-debug-subtitle\" data-if=\"call_in_progress_show_debug_subtitle\" "
           ">{{call_in_progress_debug_subtitle}}</p>";
    out << "<p class=\"text-xs shell-call-quality-hint\" data-if=\"call_in_progress_quality_hint != ''\" "
           ">{{call_in_progress_quality_hint}}</p>";
    out << "</div>";
    append_quality_chip(out, "");
    out << "<button class=\"shell-call-expand\" type=\"button\" data-event-click=\"call_expand()\">";
    out << "<svg src=\"../icons/chevron-down.svg\" width=\"18\" height=\"18\"></svg>";
    out << "</button>";
    out << "</div></div>";
    out << "<div class=\"shell-call-immersive-roster\">";
    out << "<div data-for=\"p : call_in_progress_roster\" class=\"shell-call-peer-card\">";
    out << "<call-video-tile class=\"shell-call-peer-tile\" tile=\"local\" "
           "data-if=\"p.is_local && call_in_progress_local_preview\"></call-video-tile>";
    out << "<call-video-tile class=\"shell-call-peer-tile\" tile=\"peer\" data-attr-stream=\"p.stream_id\" "
           "data-if=\"!p.is_local && p.has_remote_video\"></call-video-tile>";
    out << "<div class=\"shell-call-peer-avatar\" "
           "data-if=\"(p.is_local && !call_in_progress_local_preview) || "
           "(!p.is_local && !p.has_remote_video && !p.has_avatar)\" "
           "data-class-avatar-tone-0=\"p.avatar_tone == 0\" "
           "data-class-avatar-tone-1=\"p.avatar_tone == 1\" "
           "data-class-avatar-tone-2=\"p.avatar_tone == 2\" "
           "data-class-avatar-tone-3=\"p.avatar_tone == 3\" "
           "data-class-avatar-tone-4=\"p.avatar_tone == 4\" "
           "data-class-avatar-tone-5=\"p.avatar_tone == 5\" "
           "data-class-avatar-tone-6=\"p.avatar_tone == 6\" "
           "data-class-avatar-tone-7=\"p.avatar_tone == 7\">"
           "<p class=\"avatar-letter\">{{p.avatar_letter}}</p></div>";
    out << "<img class=\"shell-call-peer-avatar-image\" data-if=\"p.has_avatar && "
           "((p.is_local && !call_in_progress_local_preview) || (!p.is_local && !p.has_remote_video))\" "
           "data-attr-src=\"p.avatar_src\"/>";
    out << "<p class=\"text-sm shell-call-peer-name\">{{p.name}}</p>";
    out << "<p class=\"text-xs shell-call-peer-stall\" "
           "data-if=\"!p.is_local && p.video_enabled && !p.has_remote_video\" "
           ">{{call_in_progress_remote_placeholder}}</p>";
    out << "<div class=\"shell-call-peer-badges row\">";
    out << "<svg data-if=\"p.audio_muted\" src=\"../icons/mic-off.svg\" width=\"14\" height=\"14\" "
           "crop-to-content=\"true\"></svg>";
    out << "<svg data-if=\"p.video_enabled\" src=\"../icons/video.svg\" width=\"14\" height=\"14\" "
           "crop-to-content=\"true\"></svg>";
    out << "</div></div>";
    // 1:1 / empty roster: still show a single presence card from title/peer.
    out << "<div class=\"shell-call-peer-card\" data-if=\"call_in_progress_participant_count == 0\">";
    out << "<div class=\"shell-call-peer-avatar\"></div>";
    out << "<p class=\"text-sm shell-call-peer-name\">{{call_in_progress_peer_label}}</p>";
    out << "</div>";
    out << "</div>";
    out << "<div class=\"shell-call-immersive-controls\">";
    out << "<div class=\"shell-call-bar-actions row\">";
    append_core_actions(out);
    out << "</div></div></div></div>";
    return out.str();
  }

  // Expanded (default): top stage + stacked bar.
  out << "<div id=\"shell-call-chrome-root\" class=\"shell-layer shell-layer-call-bar\" data-model=\"window\">";
  out << "<div class=\"shell-call-stage\" data-if=\"call_in_progress_stage_visible\">";
  out << "<call-video-tile class=\"shell-call-remote\" id=\"call-remote-tile\" tile=\"remote\">";
  out << "<p class=\"text-sm shell-call-remote-placeholder\" data-if=\"!call_in_progress_remote_video\" "
         ">{{call_in_progress_remote_placeholder}}</p>";
  out << "</call-video-tile>";
  out << "<call-video-tile class=\"shell-call-pip\" id=\"call-local-tile\" tile=\"local\" "
         "data-if=\"call_in_progress_local_preview\"></call-video-tile>";
  out << "</div>";
  out << "<div class=\"shell-call-bar\">";
  out << "<div class=\"shell-call-bar-row row\">";
  out << "<div class=\"shell-call-bar-main\">";
  out << "<p class=\"text-sm shell-call-bar-title\">{{call_in_progress_title}}</p>";
  out << "<p class=\"text-sm shell-call-bar-subtitle\">{{call_in_progress_subtitle}}</p>";
  out << "<p class=\"text-xs shell-call-debug-subtitle\" data-if=\"call_in_progress_show_debug_subtitle\" "
         ">{{call_in_progress_debug_subtitle}}</p>";
  out << "<p class=\"text-xs shell-call-quality-hint\" data-if=\"call_in_progress_quality_hint != ''\" "
         ">{{call_in_progress_quality_hint}}</p>";
  out << "<p class=\"text-xs shell-call-bar-hint\" data-if=\"call_in_progress_show_retry\" "
         ">{{call_in_progress_status_hint}}</p>";
  out << "</div>";
  append_quality_chip(out, "");
  out << "<div class=\"shell-call-bar-actions row\">";
  append_core_actions(out);
  out << "</div></div>";
  out << "<div class=\"shell-call-roster row\" data-if=\"call_in_progress_show_roster\">";
  out << "<div data-for=\"p : call_in_progress_roster\" class=\"shell-call-roster-chip row\">";
  out << "<span class=\"text-xs shell-call-roster-name\">{{p.name}}</span>";
  out << "<svg data-if=\"p.audio_muted\" src=\"../icons/mic-off.svg\" width=\"12\" height=\"12\" "
         "crop-to-content=\"true\"></svg>";
  out << "<svg data-if=\"p.video_enabled\" src=\"../icons/video.svg\" width=\"12\" height=\"12\" "
         "crop-to-content=\"true\"></svg>";
  out << "</div></div>";
  out << "<div class=\"shell-call-bar-meters row\" data-if=\"!call_in_progress_show_roster\">";
  out << "<div class=\"shell-call-meter shell-call-meter--compact\">";
  out << "<span class=\"text-xs shell-call-meter-label\">You</span>";
  out << "<div class=\"shell-call-meter-track row\">";
  out << "<div class=\"shell-call-meter-seg\" data-class-shell-call-meter-seg--on=\"call_in_progress_mic_level >= 1\"></div>";
  out << "<div class=\"shell-call-meter-seg\" data-class-shell-call-meter-seg--on=\"call_in_progress_mic_level >= 2\"></div>";
  out << "<div class=\"shell-call-meter-seg\" data-class-shell-call-meter-seg--on=\"call_in_progress_mic_level >= 3\"></div>";
  out << "<div class=\"shell-call-meter-seg\" data-class-shell-call-meter-seg--on=\"call_in_progress_mic_level >= 4\"></div>";
  out << "<div class=\"shell-call-meter-seg\" data-class-shell-call-meter-seg--on=\"call_in_progress_mic_level >= 5\"></div>";
  out << "</div>";
  out << "<span class=\"text-xs shell-call-meter-hint\">{{call_in_progress_mic_hint}}</span>";
  out << "</div>";
  out << "<div class=\"shell-call-meter shell-call-meter--compact\">";
  out << "<span class=\"text-xs shell-call-meter-label\">{{call_in_progress_peer_label}}</span>";
  out << "<div class=\"shell-call-meter-track row\">";
  out << "<div class=\"shell-call-meter-seg\" data-class-shell-call-meter-seg--on=\"call_in_progress_peer_level >= 1\"></div>";
  out << "<div class=\"shell-call-meter-seg\" data-class-shell-call-meter-seg--on=\"call_in_progress_peer_level >= 2\"></div>";
  out << "<div class=\"shell-call-meter-seg\" data-class-shell-call-meter-seg--on=\"call_in_progress_peer_level >= 3\"></div>";
  out << "<div class=\"shell-call-meter-seg\" data-class-shell-call-meter-seg--on=\"call_in_progress_peer_level >= 4\"></div>";
  out << "<div class=\"shell-call-meter-seg\" data-class-shell-call-meter-seg--on=\"call_in_progress_peer_level >= 5\"></div>";
  out << "</div>";
  out << "<span class=\"text-xs shell-call-meter-hint\">{{call_in_progress_peer_hint}}</span>";
  out << "</div></div></div></div>";
  return out.str();
}

} // namespace pbr
