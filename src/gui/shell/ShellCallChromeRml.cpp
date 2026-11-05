#include "gui/shell/ShellCallChromeRml.h"

#include "foundation/i18n/LocalizationService.h"

#include <sstream>

namespace pbr {

namespace {

/**
 * One card per row of the bound array `rows` (data-for), with `events` on the card and its tile drawn `tile_fit`
 * ("fit": the whole frame; "fill": cover the tile, cropped).
 */
void AppendRosterCards(std::ostringstream& out, const char* rows, const char* card_class, const char* events,
                       const char* tile_fit) {
  out << "<div data-for=\"p : " << rows << "\" class=\"shell-call-peer-card " << card_class << "\" " << events << ">";
  out << "<call-video-tile class=\"shell-call-peer-tile\" tile=\"local\" fit=\"" << tile_fit
      << "\" "
         "data-if=\"p.is_local && call_in_progress_local_preview\"></call-video-tile>";
  out << "<call-video-tile class=\"shell-call-peer-tile\" tile=\"peer\" data-attr-stream=\"p.stream_id\" fit=\""
      << tile_fit << "\" data-if=\"!p.is_local && p.has_remote_video\"></call-video-tile>";
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
}

/** Text written into the markup (names, the sheet's lines): no tags, no entities, no data expressions. */
std::string Plain(const std::string& text) {
  std::string out;
  out.reserve(text.size());
  for (const char c : text) {
    if (c != '<' && c != '>' && c != '&' && c != '"' && c != '{' && c != '}') {
      out += c;
    }
  }
  return out;
}

/** A round avatar on the phone call pages: the group icon on the group colour, a photo, or the initial. */
void AppendAvatar(std::ostringstream& out, const char* size_class, const bool group, const bool has_photo,
                  const std::string& photo, const std::string& letter, const int tone) {
  if (group) {
    out << "<div class=\"call-phone-avatar call-phone-avatar--group " << size_class << "\">"
        << "<svg src=\"../icons/v2/group.svg\" width=\"48\" height=\"48\"></svg></div>";
    return;
  }
  if (has_photo) {
    out << "<img class=\"call-phone-avatar " << size_class << "\" src=\"" << Plain(photo) << "\"/>";
    return;
  }
  out << "<div class=\"call-phone-avatar avatar-tone-" << (tone & 7) << " " << size_class << "\">"
      << "<p class=\"avatar-letter\">" << Plain(letter) << "</p></div>";
}

/**
 * PP's own signal: two dots joined by a line and four bars (DESIGN-pp-ui 8.12, 2026-11-04), not the phone's cell
 * bars. Bound: it changes at most every 500 ms and dirties only its own variables. A tap opens the
 * "how this call got through" sheet; inside the sheet it is the same picture without the tap.
 */
void AppendSignal(std::ostringstream& out, const char* extra_class, const bool tappable) {
  out << "<" << (tappable ? "button type=\"button\" data-event-click=\"call_details()\"" : "div")
      << " class=\"call-phone-signal " << extra_class << "\" "
      << "data-class-call-phone-signal--fair=\"call_in_progress_signal_tone == 1\" "
      << "data-class-call-phone-signal--poor=\"call_in_progress_signal_tone == 2\">";
  out << "<div class=\"call-phone-signal-link\"><div class=\"call-phone-signal-dot\"></div>"
         "<div class=\"call-phone-signal-line\"></div><div class=\"call-phone-signal-dot\"></div></div>";
  out << "<div class=\"call-phone-signal-bars\">";
  for (int i = 1; i <= 4; ++i) {
    out << "<div class=\"call-phone-signal-bar call-phone-signal-bar--" << i << "\" "
        << "data-class-call-phone-signal-bar--on=\"call_in_progress_signal_bars >= " << i << "\"></div>";
  }
  out << "</div>";
  out << "<span class=\"call-phone-signal-word\" data-if=\"call_in_progress_signal_word != ''\">"
         "{{call_in_progress_signal_word}}</span>";
  out << (tappable ? "</button>" : "</div>");
}

/** In a call's head: the signal bar once connected, after the just-connected line has gone. */
void AppendSignalSlot(std::ostringstream& out, const char* extra_class) {
  out << "<div class=\"call-phone-signal-slot\" "
         "data-if=\"call_in_progress_signal_shown && call_in_progress_result_text == ''\">";
  AppendSignal(out, extra_class, true);
  out << "</div>";
}

/**
 * What the user can act on or should know while the call runs (the bar's old status hint: no microphone, the group
 * needs a host, the path being switched; else "you can't hear them"), and, with call diagnostics on, its technical
 * line. Under the status on every phone page.
 */
void AppendHints(std::ostringstream& out) {
  out << "<p class=\"call-phone-hint\" data-if=\"call_in_progress_status_hint != ''\">"
         "{{call_in_progress_status_hint}}</p>";
  out << "<p class=\"call-phone-hint\" data-if=\"call_in_progress_status_hint == '' && "
         "call_in_progress_quality_hint != ''\">{{call_in_progress_quality_hint}}</p>";
  out << "<p class=\"call-phone-debug\" data-if=\"call_in_progress_show_debug_subtitle\">"
         "{{call_in_progress_debug_subtitle}}</p>";
}

/** "Connected directly, 0.9 s" right after connecting; it fades after 4 s (the class's transition). */
void AppendResult(std::ostringstream& out) {
  out << "<div class=\"call-phone-result\" data-if=\"call_in_progress_result_text != ''\" "
         "data-class-call-phone-result--fading=\"call_in_progress_result_fading\">";
  out << "<span class=\"call-phone-result-check\"><svg src=\"../icons/v2/check.svg\" width=\"12\" "
         "height=\"12\"></svg></span>";
  out << "<span class=\"call-phone-result-text\">{{call_in_progress_result_text}}</span></div>";
}

/** The steps before connect: only those that happen (calling, answered, setting up the encrypted line, connected). */
void AppendSteps(std::ostringstream& out, const int stage) {
  out << "<div class=\"call-phone-steps\">";
  const char* vars[] = {"call_in_progress_story_step1", "call_in_progress_story_step2",
                        "call_in_progress_story_step3"};
  for (int i = 0; i < 3; ++i) {
    // Step 1 is current while the other side is being called, done once answered; step 2 is current after.
    const bool done = i < stage;
    const bool current = i == stage;
    out << "<div class=\"call-phone-step" << (done ? " call-phone-step--done" : current ? " call-phone-step--current" : "")
        << "\"><div class=\"call-phone-step-dot\">";
    if (done) {
      out << "<svg src=\"../icons/v2/check.svg\" width=\"12\" height=\"12\"></svg>";
    }
    out << "</div><p class=\"call-phone-step-text\">{{" << vars[i] << "}}</p></div>";
  }
  out << "</div>";
}

/** "You ↔ them" with the moving spark between them (RCSS keyframes; only while connecting). */
void AppendPair(std::ostringstream& out, const CallInProgressState& call) {
  out << "<div class=\"call-phone-pair\">";
  AppendAvatar(out, "call-phone-avatar--pair", false, call.self_has_avatar, call.self_avatar_src.c_str(),
               call.self_avatar_letter.c_str(), call.self_avatar_tone);
  out << "<div class=\"call-phone-pair-path\"><div class=\"call-phone-pair-dots\">";
  for (int i = 0; i < 9; ++i) {
    out << "<div class=\"call-phone-pair-dot\"></div>";
  }
  out << "</div><div class=\"call-phone-pair-spark\"></div></div>";
  AppendAvatar(out, "call-phone-avatar--pair", false, call.peer_has_avatar, call.peer_avatar_src.c_str(),
               call.peer_avatar_letter.c_str(), call.peer_avatar_tone);
  out << "</div>";
  out << "<div class=\"call-phone-pair-names\"><p class=\"call-phone-pair-name\">" << Tr("call.label.you")
      << "</p><p class=\"call-phone-pair-name\">{{call_in_progress_phone_title}}</p></div>";
}

/** The four true tips, one every 3 s (RCSS keyframes with staggered delays: no C++ tick). */
void AppendTips(std::ostringstream& out) {
  out << "<div class=\"call-phone-tips\">";
  int i = 1;
  for (const char* key : {"call.story.tip.encrypted", "call.story.tip.no_vpn", "call.story.tip.reroute",
                          "call.story.tip.closer"}) {
    out << "<p class=\"call-phone-tip call-phone-tip--" << i++ << "\"><span class=\"call-phone-tip-prefix\">"
        << Tr("call.story.prefix") << "</span><span class=\"call-phone-tip-text\">" << Tr(key) << "</span></p>";
  }
  out << "</div>";
}

void AppendControl(std::ostringstream& out, const char* event, const char* on_var, const std::string& icon_attr,
                   const std::string& label, const char* extra_class = "") {
  out << "<button class=\"call-phone-control " << extra_class << "\" type=\"button\" data-event-click=\"" << event
      << "\"";
  if (on_var) {
    out << " data-class-call-phone-control--on=\"" << on_var << "\"";
  }
  out << "><span class=\"call-phone-control-circle\"><svg " << icon_attr
      << " width=\"26\" height=\"26\"></svg></span>";
  out << "<span class=\"call-phone-control-label\">" << label << "</span></button>";
}

/** The bar of round buttons at the bottom (mockups 06-CallVoice / 06-CallVideo). */
void AppendControls(std::ostringstream& out, const CallInProgressState& call, const bool video_layout) {
  out << "<div class=\"call-phone-controls\">";
  const auto flip = [&]() {
    if (call.show_flip) {
      AppendControl(out, "call_flip_camera()", nullptr, "src=\"../icons/v2/camera-flip.svg\"",
                    Tr("call.control.flip"));
    }
  };
  const auto mute = [&]() {
    AppendControl(out, "call_mute()", "call_in_progress_muted",
                  "data-attr-src=\"call_in_progress_muted ? '../icons/v2/mic-off.svg' : '../icons/v2/mic.svg'\"",
                  Tr("call.control.mute"));
  };
  const auto speaker = [&]() {
    if (call.show_speaker) {
      AppendControl(out, "call_speaker()", "call_in_progress_speaker_on", "src=\"../icons/v2/speaker.svg\"",
                    Tr("call.control.speaker"));
    }
  };
  const auto camera = [&]() {
    if (call.show_camera) {
      AppendControl(out, "call_camera()", "call_in_progress_camera_on", "src=\"../icons/v2/video.svg\"",
                    "<span data-if=\"!call_in_progress_camera_on\">" + Tr("call.control.camera_on") +
                        "</span><span data-if=\"call_in_progress_camera_on\">" + Tr("call.control.camera_off") +
                        "</span>");
    }
  };
  // 1:1 couldn't connect but the page stays (the path may still move): Try again sits with the buttons, as on the
  // old bar.
  if (call.show_retry) {
    AppendControl(out, "call_retry()", nullptr, "src=\"../icons/sync.svg\"", Tr("call.failed.retry"));
  }
  if (video_layout) {
    flip();
    mute();
    camera();
    speaker();
  } else {
    mute();
    speaker();
    camera();
  }
  // Still ringing the others: the red button cancels (Kenneth 2026-11-04: a group call he started rang everyone and
  // he saw no way to cancel it); in a group it leaves, else it ends.
  const std::string end_label = call.calling      ? Tr("call.cancel_calling")
                                : call.show_roster ? Tr("call.control.leave")
                                                   : Tr("call.control.end");
  AppendControl(out, "call_leave()", nullptr, "src=\"../icons/v2/phone-hangup-fill.svg\"", end_label,
                "call-phone-control--end");
  out << "</div>";
}

/** "How this call got through": a phone sheet (DESIGN-pp-ui 8.14) over the call page. */
void AppendStorySheet(std::ostringstream& out, const CallInProgressState& call) {
  out << "<div class=\"shell-layer shell-layer-sheet call-phone-sheet-layer\">";
  out << "<div class=\"shell-scrim call-phone-sheet-scrim\" data-event-click=\"call_details()\"></div>";
  out << "<div id=\"call-story-sheet\" class=\"shell-page-sheet shell-page-sheet--large shell-page-sheet-enter "
         "call-phone-sheet\">";
  out << "<div class=\"shell-page-sheet-grabber\"></div>";
  out << "<button class=\"shell-close-btn shell-page-sheet-close\" type=\"button\" data-event-click=\"call_details()\">"
         "<span class=\"sheet-icon-disc\"><svg src=\"../icons/v2/close.svg\" width=\"12\" height=\"12\"></svg>"
         "</span></button>";
  out << "<div class=\"sheet-header\"><p class=\"sheet-title\">" << Tr("call.story.sheet.title") << "</p></div>";
  out << "<div class=\"call-phone-sheet-body\">";
  out << "<p class=\"call-phone-sheet-lead\">" << Tr("call.story.sheet.lead") << "</p>";
  out << "<div class=\"call-phone-timeline\">";
  for (const CallStoryLine& line : call.story_lines) {
    out << "<div class=\"call-phone-event" << (line.secure ? " call-phone-event--secure" : "") << "\">"
        << "<div class=\"call-phone-event-dot\"></div>"
        << "<p class=\"call-phone-event-time\">" << Plain(line.time) << "</p>"
        << "<p class=\"call-phone-event-text\">" << Plain(line.text) << "</p></div>";
  }
  out << "</div>";
  out << "<div class=\"call-phone-sheet-group\">";
  out << "<div class=\"call-phone-kv\"><p class=\"call-phone-kv-key\">" << Tr("call.story.sheet.now") << "</p>";
  AppendSignal(out, "call-phone-signal--sheet", false);
  out << "</div>";
  out << "<div class=\"call-phone-kv\"><p class=\"call-phone-kv-key\">" << Tr("call.story.sheet.how")
      << "</p><p class=\"call-phone-kv-value\">" << Plain(call.story_how.c_str()) << "</p></div>";
  out << "<div class=\"call-phone-kv call-phone-kv--last\"><p class=\"call-phone-kv-key\">"
      << Tr("call.story.sheet.encryption") << "</p><p class=\"call-phone-kv-value\">" << Tr("call.story.sheet.e2e")
      << "</p></div>";
  out << "</div></div></div></div>";
}

/**
 * The group page's people as tiles, one per row of the bound array `rows` (data-for): their camera, else their
 * photo or initial, and their name. `extra_class` sizes them (grid, spotlight, strip), `events` is the tap, `fit` how a
 * camera fills the tile ("fit" / "fill"). With `rings`, whoever speaks gets a ring (p.speaking: bound, refreshed on
 * its own at most every few hundred ms, B146).
 */
void AppendPhoneTiles(std::ostringstream& out, const char* rows, const char* extra_class, const char* events,
                      const char* fit, const bool rings) {
  out << "<div data-for=\"p : " << rows << "\" class=\"call-phone-tile" << extra_class << "\"";
  if (rings) {
    out << " data-class-call-phone-tile--speaking=\"p.speaking\"";
  }
  out << events << ">";
  out << "<call-video-tile class=\"call-phone-tile-video\" tile=\"local\"" << fit
      << " data-if=\"p.is_local && call_in_progress_local_preview\"></call-video-tile>";
  out << "<call-video-tile class=\"call-phone-tile-video\" tile=\"peer\" data-attr-stream=\"p.stream_id\"" << fit
      << " data-if=\"!p.is_local && p.has_remote_video\"></call-video-tile>";
  const char* no_video = "((p.is_local && !call_in_progress_local_preview) || (!p.is_local && !p.has_remote_video))";
  out << "<div class=\"call-phone-avatar call-phone-avatar--tile\" data-if=\"!p.has_avatar && " << no_video << "\"";
  for (int tone = 0; tone < 8; ++tone) {
    out << " data-class-avatar-tone-" << tone << "=\"p.avatar_tone == " << tone << "\"";
  }
  out << "><p class=\"avatar-letter\">{{p.avatar_letter}}</p></div>";
  out << "<img class=\"call-phone-avatar call-phone-avatar--tile\" data-if=\"p.has_avatar && " << no_video
      << "\" data-attr-src=\"p.avatar_src\"/>";
  out << "<p class=\"call-phone-tile-note\" data-if=\"!p.is_local && p.video_enabled && !p.has_remote_video\">"
         "{{call_in_progress_remote_placeholder}}</p>";
  out << "<div class=\"call-phone-tile-label\"><svg data-if=\"p.audio_muted\" src=\"../icons/v2/mic-off.svg\" "
         "width=\"14\" height=\"14\"></svg><span class=\"call-phone-tile-name\">{{p.name}}</span></div>";
  out << "</div>";
}

/** The phone ring page (mockups 06-CallRing / 06-CallGroupRing): full screen, the caller big, three round answers. */
std::string CallRingPhoneRml(const ShellState& state) {
  const CallRingState& ring = state.call_ring;
  std::ostringstream out;
  out << "<div id=\"call-phone-ring\" class=\"shell-layer call-phone call-phone--ring\" data-model=\"window\">";
  out << "<div class=\"call-phone-hero call-phone-hero--ring\">";
  AppendAvatar(out, "call-phone-avatar--big", ring.group, ring.has_avatar, ring.avatar_src.c_str(),
               ring.avatar_letter.c_str(), ring.avatar_tone);
  out << "<p class=\"call-phone-name\">{{call_ring_phone_title}}</p>";
  out << "<p class=\"call-phone-status\">{{call_ring_phone_subtitle}}</p>";
  out << "<p class=\"call-phone-hint\" data-if=\"call_ring_conflict\">{{call_ring_conflict_hint}}</p>";
  out << "<p class=\"call-phone-hint\" data-if=\"call_ring_show_pricing\">{{call_ring_pricing_label}}</p>";
  out << "</div>";
  out << "<div class=\"call-phone-answer\">";
  out << "<div class=\"call-phone-answer-action\"><button class=\"call-phone-round call-phone-round--decline\" "
         "type=\"button\" data-event-click=\"call_decline()\"><svg src=\"../icons/v2/phone-hangup-fill.svg\" "
         "width=\"34\" height=\"34\"></svg></button>"
         "<p class=\"call-phone-answer-label\">{{call_ring_decline_label}}</p></div>";
  out << "<div class=\"call-phone-answer-action\" data-if=\"call_ring_video_allowed\"><button class=\"call-phone-round "
         "call-phone-round--accept\" type=\"button\" data-event-click=\"call_accept_voice()\"><svg "
         "src=\"../icons/v2/phone.svg\" width=\"30\" height=\"30\"></svg></button>"
         "<p class=\"call-phone-answer-label\">{{call_ring_voice_answer_label}}</p></div>";
  out << "<div class=\"call-phone-answer-action\"><button class=\"call-phone-round call-phone-round--accept\" "
         "type=\"button\" data-event-click=\"call_accept()\"><svg width=\"30\" height=\"30\" "
         "data-attr-src=\"call_ring_video_allowed ? '../icons/v2/video.svg' : '../icons/v2/phone.svg'\"></svg></button>"
         "<p class=\"call-phone-answer-label\">{{call_ring_accept_label}}</p></div>";
  out << "</div>";
  out << "<div class=\"call-phone-charge\" data-if=\"call_ring_show_pricing\">";
  out << "<button class=\"call-phone-pill-btn\" type=\"button\" "
         "data-attrif-disabled=\"!call_ring_accept_charge_enabled\" "
         "data-event-click=\"call_accept_charge()\">{{call_ring_accept_charge_label}}</button>";
  out << "<p class=\"call-phone-hint\" data-if=\"!call_ring_accept_charge_enabled\">{{call_ring_accept_charge_hint}}</p>";
  out << "</div></div>";
  return out.str();
}

/**
 * The phone call page (DESIGN-pp-ui 8.12 with its 2026-11-04 connection story; mockups 06-CallVoice / 06-CallVideo /
 * 06-CallFailed / 06-CallGroupVoice / 06-CallGroupVideo and 08-call-connect-story). Full screen in both open modes;
 * the chevron (or a pull down) shrinks the call to its corner chip. What the story says is decided in
 * CallConnectStory; the animations are RCSS keyframes and exist only in the markup written before connect.
 */
std::string CallPhoneRml(const ShellState& state) {
  const CallInProgressState& call = state.call_in_progress;
  const bool group = call.show_roster;
  const bool video_layout = !group && call.stage_visible && !call.phone_failed;
  // Group, connected: someone spotlit (CallSpotlight.h, V059) fills the area and the others line up in a strip.
  const bool spotlit = group && !call.story_active && !call.phone_failed && !call.spotlight_stream.empty();
  // 1:1 video: our own camera and theirs, which is big; a tap on the small one swaps them (V059).
  std::string self_stream;
  std::string peer_stream;
  for (const CallRosterParticipantState& row : call.roster) {
    (row.is_local ? self_stream : peer_stream) = Plain(row.stream_id.c_str());
  }
  const bool self_big = !group && !self_stream.empty() && call.spotlight_stream.c_str() == self_stream;
  std::ostringstream out;
  out << "<div id=\"shell-call-chrome-root\" class=\"shell-layer shell-layer-call-phone\" data-model=\"window\">";
  out << "<div id=\"call-phone-page\" class=\"call-phone" << (video_layout ? " call-phone--video" : "")
      << (group ? " call-phone--group" : "") << "\">";

  if (video_layout) {
    if (call.story_active) {
      // Before connect: the step card floats on our own camera, dimmed.
      out << "<call-video-tile class=\"call-phone-remote\" tile=\"local\" "
             "data-if=\"call_in_progress_local_preview\"></call-video-tile>";
      out << "<div class=\"call-phone-video-dim\"></div>";
    } else {
      // Swapped: our camera big while it is on (theirs is big again once it goes off).
      if (self_big) {
        out << "<call-video-tile class=\"call-phone-remote\" tile=\"local\" "
               "data-if=\"call_in_progress_local_preview\"></call-video-tile>";
      }
      out << "<call-video-tile class=\"call-phone-remote\" id=\"call-remote-tile\" tile=\"remote\""
          << (self_big ? " data-if=\"!call_in_progress_local_preview\"" : "") << ">";
      out << "<p class=\"call-phone-remote-note\" data-if=\"!call_in_progress_remote_video\">"
             "{{call_in_progress_remote_placeholder}}</p>";
      out << "</call-video-tile>";
      out << "<div class=\"call-phone-video-top\"></div>";
    }
  }

  // Top row: shrink, "end-to-end encrypted", back to the grid (with a spotlight), add people.
  out << "<div class=\"call-phone-top\">";
  const bool invite = call.show_invite && !call.phone_failed;
  const bool wide_sides = spotlit && invite;
  out << (wide_sides ? "<div class=\"call-phone-top-side\">" : "");
  out << "<button class=\"call-phone-top-btn\" type=\"button\" data-event-click=\"call_minimize()\">"
         "<svg src=\"../icons/v2/chevron-down.svg\" width=\"26\" height=\"26\"></svg></button>";
  out << (wide_sides ? "</div>" : "");
  out << "<div class=\"call-phone-sealed\"><svg src=\"../icons/v2/lock.svg\" width=\"12\" height=\"12\"></svg>"
         "<span class=\"call-phone-sealed-text\">" << Tr("call.top.e2e") << "</span></div>";
  // With a spotlight, the grid button sits before add-people; the chevron's side widens to match, keeping the
  // middle centred.
  if (wide_sides) {
    out << "<div class=\"call-phone-top-side call-phone-top-side--end\">";
  }
  if (spotlit) {
    out << "<button class=\"call-phone-top-btn\" type=\"button\" data-event-click=\"call_spotlight_grid()\">"
           "<svg src=\"../icons/grid.svg\" width=\"22\" height=\"22\"></svg></button>";
  }
  if (invite) {
    out << "<button class=\"call-phone-top-btn\" type=\"button\" data-event-click=\"call_invite()\">"
           "<svg src=\"../icons/v2/user-plus.svg\" width=\"24\" height=\"24\"></svg></button>";
  } else if (!spotlit) {
    out << "<div class=\"call-phone-top-spacer\"></div>";
  }
  if (wide_sides) {
    out << "</div>";
  }
  // The top pill (DESIGN-pp-ui 8.11) hangs under this row: a slower line, a reconnect, a closer way; gone after 3 s.
  out << "<div class=\"call-phone-notice\" data-if=\"call_in_progress_notice_text != ''\" "
         "data-class-call-phone-notice--warn=\"call_in_progress_notice_warn\" "
         "data-class-call-phone-notice--fading=\"call_in_progress_notice_fading\">";
  out << "<span class=\"call-phone-notice-icon\"><svg src=\"../icons/v2/info.svg\" width=\"16\" height=\"16\">"
         "</svg></span><p class=\"call-phone-notice-text\">{{call_in_progress_notice_text}}</p></div>";
  out << "</div>";

  if (call.phone_failed) {
    out << "<div class=\"call-phone-hero\">";
    AppendAvatar(out, "call-phone-avatar--big", group, !group && call.peer_has_avatar, call.peer_avatar_src.c_str(),
                 call.peer_avatar_letter.c_str(), call.peer_avatar_tone);
    out << "<p class=\"call-phone-name\">{{call_in_progress_phone_title}}</p>";
    out << "<p class=\"call-phone-status\">{{call_in_progress_failed_text}}</p>";
    out << "<p class=\"call-phone-hint\" data-if=\"call_in_progress_status_hint != ''\">"
           "{{call_in_progress_status_hint}}</p>";
    out << "</div>";
    out << "<div class=\"call-phone-failed-actions\">";
    if (call.show_retry) {
      out << "<button class=\"call-phone-pill-btn call-phone-pill-btn--primary\" type=\"button\" "
             "data-event-click=\"call_retry()\">" << Tr("call.failed.retry") << "</button>";
    }
    out << "<button class=\"call-phone-pill-btn\" type=\"button\" data-event-click=\"call_leave()\">"
        << Tr("call.failed.close") << "</button>";
    out << "</div></div></div>";
    return out.str();
  }

  if (group) {
    out << "<div class=\"call-phone-head\">";
    out << "<p class=\"call-phone-group-name\">{{call_in_progress_phone_title}}</p>";
    out << "<div class=\"call-phone-sub-row\"><p class=\"call-phone-sub\">{{call_in_progress_subtitle}}</p>";
    AppendSignalSlot(out, "call-phone-signal--compact");
    out << "</div>";
    AppendHints(out);
    out << "</div>";
    out << "<div class=\"call-phone-group-body\">";
    if (!spotlit) {
      // The 2x2 grid; once connected a tap on someone spotlights them (V059). Before connect: dimmed under the steps.
      out << "<div class=\"call-phone-grid" << (call.story_active ? " call-phone-grid--dim" : "") << "\">";
      AppendPhoneTiles(out, "call_in_progress_roster", "",
                       call.story_active ? "" : " data-event-click=\"call_spotlight(p.stream_id)\"", "",
                       !call.story_active);
      out << "</div>";
    } else {
      // The spotlit person (the one speaking, or the one tapped) fills the area; a double tap: whole frame / cropped.
      out << "<div class=\"call-phone-spotlight\">";
      AppendPhoneTiles(out, "call_in_progress_spotlight_rows", " call-phone-tile--spotlit",
                       " data-event-dblclick=\"call_spotlight_fit()\"",
                       call.spotlight_fill ? " fit=\"fill\"" : " fit=\"fit\"", true);
      out << "</div>";
    }
    if (call.story_active) {
      out << "<div class=\"call-phone-story call-phone-story--card call-phone-story--group\"><div class=\"call-phone-card\">";
      AppendSteps(out, call.story_stage);
      out << "</div></div>";
    }
    out << "</div>";
    if (spotlit) {
      // Everyone else in a row of small tiles above the buttons; a tap spotlights them instead.
      out << "<div class=\"call-phone-strip\">";
      AppendPhoneTiles(out, "call_in_progress_strip_rows", " call-phone-tile--strip",
                       " data-event-click=\"call_spotlight(p.stream_id)\"", " fit=\"fill\"", true);
      out << "</div>";
    }
  } else if (video_layout && !call.story_active) {
    out << "<div class=\"call-phone-video-body\"><div class=\"call-phone-who\">";
    out << "<p class=\"call-phone-who-name\">{{call_in_progress_phone_title}}</p>";
    out << "<p class=\"call-phone-who-status\">{{call_in_progress_subtitle}}</p>";
    AppendResult(out);
    AppendSignalSlot(out, "call-phone-signal--compact");
    AppendHints(out);
    out << "</div>";
    // The small picture: ours, or theirs when swapped; a tap swaps (spotlights the one it shows).
    const std::string swap_to = self_big ? peer_stream : self_stream;
    const std::string swap =
        swap_to.empty() ? std::string{} : " data-event-click=\"call_spotlight('" + swap_to + "')\"";
    out << "<call-video-tile class=\"call-phone-pip\""
        << (self_big ? " tile=\"remote\"" : " id=\"call-local-tile\" tile=\"local\"")
        << " data-if=\"call_in_progress_local_preview\"" << swap << "></call-video-tile></div>";
  } else if (call.story_active) {
    out << "<div class=\"call-phone-story" << (video_layout ? " call-phone-story--card" : "") << "\">";
    out << (video_layout ? "<div class=\"call-phone-card\">" : "");
    AppendPair(out, call);
    AppendSteps(out, call.story_stage);
    AppendHints(out);
    out << (video_layout ? "</div>" : "");
    out << "</div>";
  } else {
    out << "<div class=\"call-phone-hero\">";
    AppendAvatar(out, "call-phone-avatar--big", false, call.peer_has_avatar, call.peer_avatar_src.c_str(),
                 call.peer_avatar_letter.c_str(), call.peer_avatar_tone);
    out << "<p class=\"call-phone-name\">{{call_in_progress_phone_title}}</p>";
    out << "<p class=\"call-phone-status\">{{call_in_progress_subtitle}}</p>";
    AppendResult(out);
    AppendSignalSlot(out, "");
    AppendHints(out);
    out << "</div>";
  }

  if (call.story_active && !group) {
    AppendTips(out);
  }

  AppendControls(out, call, video_layout);
  out << "</div>";
  if (call.story_sheet_open) {
    AppendStorySheet(out, call);
  }
  out << "</div>";
  return out.str();
}

} // namespace

std::string CallRingRml(const ShellState& state) {
  // Mounted into #shell-call-ring-mount when active (RemountCallChrome). Presence = visibility;
  // data-if alone was not flipping Display on idle Present (accept dialog stayed gone).
  if (!state.call_ring.active) {
    return {};
  }
  if (state.layout_mode == LayoutMode::Compact) {
    return CallRingPhoneRml(state);
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
    out << "<button class=\"shell-call-flip\" type=\"button\" data-if=\"call_in_progress_show_flip\" "
           "data-event-click=\"call_flip_camera()\">";
    out << "<svg src=\"../icons/camera-flip.svg\" width=\"18\" height=\"18\" crop-to-content=\"true\"></svg>";
    out << "</button>";
    out << "<button class=\"shell-call-leave\" type=\"button\" data-class-shell-call-leave--ringing=\"call_in_progress_calling\" "
           "data-event-click=\"call_leave()\">";
    out << "<svg src=\"../icons/phone-hangup.svg\" width=\"18\" height=\"18\" crop-to-content=\"true\"></svg>";
    out << "</button>";
  };

  // An outgoing call still ringing: a labelled Cancel under the controls (phones only, by style), the End button's
  // tap. Kenneth 2026-11-04: a group call he started rang everyone and he saw no way to cancel it.
  auto append_cancel = [](std::ostringstream& out) {
    out << "<div class=\"shell-call-cancel-row\" data-if=\"call_in_progress_calling\">";
    out << "<button class=\"shell-call-cancel\" type=\"button\" data-event-click=\"call_leave()\">";
    out << "<svg src=\"../icons/phone-hangup.svg\" width=\"22\" height=\"22\" crop-to-content=\"true\"></svg>";
    out << "<span class=\"shell-call-cancel-label\">" << Tr("call.cancel_calling") << "</span>";
    out << "</button></div>";
  };

  const auto& call = state.call_in_progress;
  if (state.layout_mode == LayoutMode::Compact && call.mode != CallChromeMode::Minimized) {
    return CallPhoneRml(state);
  }
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
    if (!call.spotlight_stream.empty()) {
      out << "<button class=\"shell-call-grid\" type=\"button\" data-event-click=\"call_spotlight_grid()\">";
      out << "<svg src=\"../icons/grid.svg\" width=\"18\" height=\"18\" crop-to-content=\"true\"></svg>";
      out << "</button>";
    }
    out << "<button class=\"shell-call-expand\" type=\"button\" data-event-click=\"call_expand()\">";
    out << "<svg src=\"../icons/chevron-down.svg\" width=\"18\" height=\"18\"></svg>";
    out << "</button>";
    out << "</div></div>";
    if (call.spotlight_stream.empty()) {
      // The grid: a tap on a card spotlights that person (V059).
      out << "<div class=\"shell-call-immersive-roster\">";
      AppendRosterCards(out, "call_in_progress_roster", "", "data-event-click=\"call_spotlight(p.stream_id)\"", "fit");
      // 1:1 / empty roster: still show a single presence card from title/peer.
      out << "<div class=\"shell-call-peer-card\" data-if=\"call_in_progress_participant_count == 0\">";
      out << "<div class=\"shell-call-peer-avatar\"></div>";
      out << "<p class=\"text-sm shell-call-peer-name\">{{call_in_progress_peer_label}}</p>";
      out << "</div>";
      out << "</div>";
    } else {
      // The spotlight fills the call area (a double tap: fill / fit); the others line up in a strip, a tap on one
      // spotlights them instead.
      out << "<div class=\"shell-call-immersive-spotlight\">";
      AppendRosterCards(out, "call_in_progress_spotlight_rows", "shell-call-peer-card--spotlit",
                        "data-event-dblclick=\"call_spotlight_fit()\"", call.spotlight_fill ? "fill" : "fit");
      out << "</div>";
      out << "<div class=\"shell-call-immersive-strip\">";
      AppendRosterCards(out, "call_in_progress_strip_rows", "shell-call-peer-card--strip",
                        "data-event-click=\"call_spotlight(p.stream_id)\"", "fill");
      out << "</div>";
    }
    out << "<div class=\"shell-call-immersive-controls\">";
    out << "<div class=\"shell-call-bar-actions row\">";
    append_core_actions(out);
    out << "</div>";
    append_cancel(out);
    out << "</div></div></div>";
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
  append_cancel(out);
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
