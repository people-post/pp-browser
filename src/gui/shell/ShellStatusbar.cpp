#include "gui/shell/ShellStatusbar.h"

#include "foundation/i18n/LocalizationService.h"
#include "foundation/platform/Platform.h"
#include "foundation/platform/ui/RmlUi_Backend.h"
#include "gui/shell/DataModelHost.h"
#include "lib/ui/ContextMenuHost.h"
#include "lib/ui/DataEvent.h"

#include <ui/data/DataModelHandle.h>
#include <ui/dom/Context.h>
#include <ui/dom/Element.h>
#include <ui/dom/ElementDocument.h>

#include <algorithm>
#include <cmath>
#include <utility>

namespace pbr {

ShellStatusbar::ShellStatusbar(ShellState& state, ui::Context*& context, const MessagingShellPorts& ports,
                               std::function<void()> apply_safe_area_layout)
    : state_(state), context_(context), ports_(ports), apply_safe_area_layout_(std::move(apply_safe_area_layout)) {}

void ShellStatusbar::Bind(ui::DataModelConstructor& ctor) {
    ctor.Bind("statusbar_visible", &state_.statusbar_visible);
    ctor.Bind("statusbar_brief_visible", &state_.statusbar_brief_visible);
    ctor.Bind("statusbar_brief_ok", &state_.statusbar_brief_ok);
    ctor.Bind("statusbar_brief_failed", &state_.statusbar_brief_failed);
    ctor.Bind("statusbar_brief_unknown", &state_.statusbar_brief_unknown);
    ctor.Bind("statusbar_direct_visible", &state_.statusbar_direct_visible);
    ctor.Bind("statusbar_direct_ok", &state_.statusbar_direct_ok);
    ctor.Bind("statusbar_direct_off", &state_.statusbar_direct_off);
    ctor.Bind("statusbar_direct_error", &state_.statusbar_direct_error);
    ctor.Bind("statusbar_direct_checking", &state_.statusbar_direct_checking);
    ctor.Bind("statusbar_help_visible", &state_.statusbar_help_visible);
    ctor.Bind("statusbar_inbound_visible", &state_.statusbar_inbound_visible);
    ctor.Bind("statusbar_inbound_ok", &state_.statusbar_inbound_ok);
    ctor.Bind("statusbar_inbound_off", &state_.statusbar_inbound_off);
    ctor.Bind("statusbar_load_circuit_visible", &state_.statusbar_load_circuit_visible);
    ctor.Bind("statusbar_load_media_visible", &state_.statusbar_load_media_visible);
    ctor.Bind("statusbar_load_circuit_label", &state_.statusbar_load_circuit_label);
    ctor.Bind("statusbar_load_media_label", &state_.statusbar_load_media_label);
    ctor.Bind("statusbar_load_circuit_title", &state_.statusbar_load_circuit_title);
    ctor.Bind("statusbar_load_media_title", &state_.statusbar_load_media_title);
    ctor.Bind("statusbar_label", &state_.statusbar_label);
    ctor.Bind("statusbar_label_warn", &state_.statusbar_label_warn);
    ctor.Bind("statusbar_label_error", &state_.statusbar_label_error);
    ctor.Bind("statusbar_brief_title", &state_.statusbar_brief_title);
    ctor.Bind("statusbar_direct_title", &state_.statusbar_direct_title);
    ctor.Bind("statusbar_help_title", &state_.statusbar_help_title);
    ctor.Bind("statusbar_inbound_title", &state_.statusbar_inbound_title);
    ctor.Bind("statusbar_cluster_title", &state_.statusbar_cluster_title);
    ctor.Bind("statusbar_popover_open", &state_.statusbar_popover_open);
    ctor.Bind("statusbar_popover_brief_label", &state_.statusbar_popover_brief_label);
    ctor.Bind("statusbar_popover_direct_label", &state_.statusbar_popover_direct_label);
    ctor.Bind("statusbar_popover_reach_label", &state_.statusbar_popover_reach_label);
    ctor.Bind("statusbar_popover_reach_summary", &state_.statusbar_popover_reach_summary);
    ctor.Bind("statusbar_popover_help_visible", &state_.statusbar_popover_help_visible);
    ctor.Bind("statusbar_popover_help_label", &state_.statusbar_popover_help_label);
    ctor.Bind("statusbar_popover_upnp_visible", &state_.statusbar_popover_upnp_visible);
    ctor.Bind("statusbar_popover_upnp_label", &state_.statusbar_popover_upnp_label);
    ctor.Bind("statusbar_popover_error_visible", &state_.statusbar_popover_error_visible);
    ctor.Bind("statusbar_popover_error", &state_.statusbar_popover_error);
    ctor.Bind("statusbar_popover_load_visible", &state_.statusbar_popover_load_visible);
    ctor.Bind("statusbar_popover_circuit_load", &state_.statusbar_popover_circuit_load);
    ctor.Bind("statusbar_popover_media_sessions", &state_.statusbar_popover_media_sessions);
    ctor.Bind("statusbar_popover_media_participants", &state_.statusbar_popover_media_participants);
    ctor.Bind("statusbar_popover_network_visible", &state_.statusbar_popover_network_visible);
    ctor.Bind("statusbar_popover_network_links", &state_.statusbar_popover_network_links);
    ctor.Bind("statusbar_popover_network_parked", &state_.statusbar_popover_network_parked);
    ctor.Bind("statusbar_popover_network_rate", &state_.statusbar_popover_network_rate);
    ctor.Bind("statusbar_popover_network_rtt", &state_.statusbar_popover_network_rtt);
    ctor.Bind("statusbar_popover_network_resend", &state_.statusbar_popover_network_resend);
    ctor.Bind("statusbar_popover_relay_rate", &state_.statusbar_popover_relay_rate);
    ctor.BindEventCallback("toggle_statusbar_popover", DataEvent<>([this]() { TogglePopover(); }));
    ctor.BindEventCallback("dismiss_statusbar_popover", DataEvent<>([this]() { ClosePopover(); }));
    ctor.BindEventCallback("retest_statusbar_reachability", DataEvent<>([this]() { RetestReachability(); }));
    ctor.BindEventCallback("open_network_settings", DataEvent<>([this]() { OpenNetworkSettings(); }));
}

void ShellStatusbar::Dirty() {
  DataModelHost::Instance().Dirty("window", "activity_visible");
  DataModelHost::Instance().Dirty("window", "statusbar_visible");
  DataModelHost::Instance().Dirty("window", "statusbar_brief_visible");
  DataModelHost::Instance().Dirty("window", "statusbar_brief_ok");
  DataModelHost::Instance().Dirty("window", "statusbar_brief_failed");
  DataModelHost::Instance().Dirty("window", "statusbar_brief_unknown");
  DataModelHost::Instance().Dirty("window", "statusbar_direct_visible");
  DataModelHost::Instance().Dirty("window", "statusbar_direct_ok");
  DataModelHost::Instance().Dirty("window", "statusbar_direct_off");
  DataModelHost::Instance().Dirty("window", "statusbar_direct_error");
  DataModelHost::Instance().Dirty("window", "statusbar_direct_checking");
  DataModelHost::Instance().Dirty("window", "statusbar_help_visible");
  DataModelHost::Instance().Dirty("window", "statusbar_inbound_visible");
  DataModelHost::Instance().Dirty("window", "statusbar_inbound_ok");
  DataModelHost::Instance().Dirty("window", "statusbar_inbound_off");
  DataModelHost::Instance().Dirty("window", "statusbar_load_circuit_visible");
  DataModelHost::Instance().Dirty("window", "statusbar_load_media_visible");
  DataModelHost::Instance().Dirty("window", "statusbar_load_circuit_label");
  DataModelHost::Instance().Dirty("window", "statusbar_load_media_label");
  DataModelHost::Instance().Dirty("window", "statusbar_load_circuit_title");
  DataModelHost::Instance().Dirty("window", "statusbar_load_media_title");
  DataModelHost::Instance().Dirty("window", "statusbar_label");
  DataModelHost::Instance().Dirty("window", "statusbar_label_warn");
  DataModelHost::Instance().Dirty("window", "statusbar_label_error");
  DataModelHost::Instance().Dirty("window", "statusbar_activity");
  DataModelHost::Instance().Dirty("window", "statusbar_brief_title");
  DataModelHost::Instance().Dirty("window", "statusbar_direct_title");
  DataModelHost::Instance().Dirty("window", "statusbar_help_title");
  DataModelHost::Instance().Dirty("window", "statusbar_inbound_title");
  DataModelHost::Instance().Dirty("window", "statusbar_cluster_title");
  DirtyPopover();
  DataModelHost::Instance().Dirty("window", "titlebar_visible");
  DataModelHost::Instance().Dirty("window", "titlebar_traffic_lights");
  DataModelHost::Instance().Dirty("window", "window_maximized");
  DataModelHost::Instance().Dirty("window", "fonts_ready");
}

void ShellStatusbar::DirtyPopover() {
  DataModelHost::Instance().Dirty("window", "statusbar_popover_open");
  DataModelHost::Instance().Dirty("window", "statusbar_popover_brief_label");
  DataModelHost::Instance().Dirty("window", "statusbar_popover_direct_label");
  DataModelHost::Instance().Dirty("window", "statusbar_popover_reach_label");
  DataModelHost::Instance().Dirty("window", "statusbar_popover_reach_summary");
  DataModelHost::Instance().Dirty("window", "statusbar_popover_help_visible");
  DataModelHost::Instance().Dirty("window", "statusbar_popover_help_label");
  DataModelHost::Instance().Dirty("window", "statusbar_popover_upnp_visible");
  DataModelHost::Instance().Dirty("window", "statusbar_popover_upnp_label");
  DataModelHost::Instance().Dirty("window", "statusbar_popover_error_visible");
  DataModelHost::Instance().Dirty("window", "statusbar_popover_error");
  DataModelHost::Instance().Dirty("window", "statusbar_popover_load_visible");
  DataModelHost::Instance().Dirty("window", "statusbar_popover_circuit_load");
  DataModelHost::Instance().Dirty("window", "statusbar_popover_media_sessions");
  DataModelHost::Instance().Dirty("window", "statusbar_popover_media_participants");
  DataModelHost::Instance().Dirty("window", "statusbar_popover_network_visible");
  DataModelHost::Instance().Dirty("window", "statusbar_popover_network_links");
  DataModelHost::Instance().Dirty("window", "statusbar_popover_network_parked");
  DataModelHost::Instance().Dirty("window", "statusbar_popover_network_rate");
  DataModelHost::Instance().Dirty("window", "statusbar_popover_network_rtt");
  DataModelHost::Instance().Dirty("window", "statusbar_popover_network_resend");
  DataModelHost::Instance().Dirty("window", "statusbar_popover_relay_rate");
}

void ShellStatusbar::RefreshVisibility() {
  const bool visible = Platform::IsDesktop() && state_.layout_mode == LayoutMode::Expanded;
  if (visible == state_.statusbar_visible) {
    return;
  }
  state_.statusbar_visible = visible;
  if (!visible && state_.statusbar_popover_open) {
    ClosePopover();
  }
  Dirty();
  apply_safe_area_layout_();
}

void ShellStatusbar::ClearCluster() {
  state_.statusbar_brief_visible = false;
  state_.statusbar_brief_ok = false;
  state_.statusbar_brief_failed = false;
  state_.statusbar_brief_unknown = false;
  state_.statusbar_direct_visible = false;
  state_.statusbar_direct_ok = false;
  state_.statusbar_direct_off = false;
  state_.statusbar_direct_error = false;
  state_.statusbar_direct_checking = false;
  state_.statusbar_help_visible = false;
  state_.statusbar_inbound_visible = false;
  state_.statusbar_inbound_ok = false;
  state_.statusbar_inbound_off = false;
  state_.statusbar_load_circuit_visible = false;
  state_.statusbar_load_media_visible = false;
  state_.statusbar_load_circuit_label.clear();
  state_.statusbar_load_media_label.clear();
  state_.statusbar_load_circuit_title.clear();
  state_.statusbar_load_media_title.clear();
  state_.statusbar_label.clear();
  state_.statusbar_label_warn = false;
  state_.statusbar_label_error = false;
  state_.statusbar_brief_title.clear();
  state_.statusbar_direct_title.clear();
  state_.statusbar_help_title.clear();
  state_.statusbar_inbound_title.clear();
  state_.statusbar_cluster_title.clear();
}

bool ShellStatusbar::ApplyCluster(const StatusbarClusterSnapshot& snap) {
  const bool brief_visible = snap.brief != StatusbarClusterSnapshot::BriefState::Hidden;
  const bool brief_ok = snap.brief == StatusbarClusterSnapshot::BriefState::Ok;
  const bool brief_failed = snap.brief == StatusbarClusterSnapshot::BriefState::Failed;
  const bool brief_unknown = snap.brief == StatusbarClusterSnapshot::BriefState::Unknown;

  const bool direct_visible = snap.direct != StatusbarClusterSnapshot::DirectState::Hidden;
  const bool direct_ok = snap.direct == StatusbarClusterSnapshot::DirectState::On;
  const bool direct_off = snap.direct == StatusbarClusterSnapshot::DirectState::Off;
  const bool direct_error = snap.direct == StatusbarClusterSnapshot::DirectState::Error;
  const bool direct_checking = snap.direct == StatusbarClusterSnapshot::DirectState::Checking;

  const bool inbound_visible = snap.inbound != StatusbarClusterSnapshot::InboundState::Hidden;
  const bool inbound_ok = snap.inbound == StatusbarClusterSnapshot::InboundState::On;
  const bool inbound_off = snap.inbound == StatusbarClusterSnapshot::InboundState::Off;

  const ui::String label = snap.label.c_str();
  const bool label_warn = snap.label_tone == StatusbarClusterSnapshot::LabelTone::Warn;
  const bool label_error = snap.label_tone == StatusbarClusterSnapshot::LabelTone::Error;
  const ui::String brief_title = snap.brief_title.c_str();
  const ui::String direct_title = snap.direct_title.c_str();
  const ui::String help_title = snap.help_title.c_str();
  const ui::String inbound_title = snap.inbound_title.c_str();
  const ui::String cluster_title = Tr("shell.statusbar.a11y.cluster").c_str();
  const ui::String load_circuit = snap.load_circuit_label.c_str();
  const ui::String load_media = snap.load_media_label.c_str();
  const ui::String load_circuit_title = snap.load_circuit_title.c_str();
  const ui::String load_media_title = snap.load_media_title.c_str();

  if (state_.statusbar_brief_visible == brief_visible && state_.statusbar_brief_ok == brief_ok &&
      state_.statusbar_brief_failed == brief_failed &&
      state_.statusbar_brief_unknown == brief_unknown &&
      state_.statusbar_direct_visible == direct_visible && state_.statusbar_direct_ok == direct_ok &&
      state_.statusbar_direct_off == direct_off && state_.statusbar_direct_error == direct_error &&
      state_.statusbar_direct_checking == direct_checking &&
      state_.statusbar_help_visible == snap.help_visible &&
      state_.statusbar_inbound_visible == inbound_visible &&
      state_.statusbar_inbound_ok == inbound_ok && state_.statusbar_inbound_off == inbound_off &&
      state_.statusbar_load_circuit_visible == snap.load_circuit_visible &&
      state_.statusbar_load_media_visible == snap.load_media_visible &&
      state_.statusbar_load_circuit_label == load_circuit &&
      state_.statusbar_load_media_label == load_media &&
      state_.statusbar_load_circuit_title == load_circuit_title &&
      state_.statusbar_load_media_title == load_media_title &&
      state_.statusbar_label == label && state_.statusbar_label_warn == label_warn &&
      state_.statusbar_label_error == label_error && state_.statusbar_brief_title == brief_title &&
      state_.statusbar_direct_title == direct_title && state_.statusbar_help_title == help_title &&
      state_.statusbar_inbound_title == inbound_title &&
      state_.statusbar_cluster_title == cluster_title) {
    return false;
  }

  state_.statusbar_brief_visible = brief_visible;
  state_.statusbar_brief_ok = brief_ok;
  state_.statusbar_brief_failed = brief_failed;
  state_.statusbar_brief_unknown = brief_unknown;
  state_.statusbar_direct_visible = direct_visible;
  state_.statusbar_direct_ok = direct_ok;
  state_.statusbar_direct_off = direct_off;
  state_.statusbar_direct_error = direct_error;
  state_.statusbar_direct_checking = direct_checking;
  state_.statusbar_help_visible = snap.help_visible;
  state_.statusbar_inbound_visible = inbound_visible;
  state_.statusbar_inbound_ok = inbound_ok;
  state_.statusbar_inbound_off = inbound_off;
  state_.statusbar_load_circuit_visible = snap.load_circuit_visible;
  state_.statusbar_load_media_visible = snap.load_media_visible;
  state_.statusbar_load_circuit_label = load_circuit;
  state_.statusbar_load_media_label = load_media;
  state_.statusbar_load_circuit_title = load_circuit_title;
  state_.statusbar_load_media_title = load_media_title;
  state_.statusbar_label = label;
  state_.statusbar_label_warn = label_warn;
  state_.statusbar_label_error = label_error;
  state_.statusbar_brief_title = brief_title;
  state_.statusbar_direct_title = direct_title;
  state_.statusbar_help_title = help_title;
  state_.statusbar_inbound_title = inbound_title;
  state_.statusbar_cluster_title = cluster_title;
  return true;
}

void ShellStatusbar::ClearPopover() {
  state_.statusbar_popover_brief_label.clear();
  state_.statusbar_popover_direct_label.clear();
  state_.statusbar_popover_reach_label.clear();
  state_.statusbar_popover_reach_summary.clear();
  state_.statusbar_popover_help_visible = false;
  state_.statusbar_popover_help_label.clear();
  state_.statusbar_popover_upnp_visible = false;
  state_.statusbar_popover_upnp_label.clear();
  state_.statusbar_popover_error_visible = false;
  state_.statusbar_popover_error.clear();
  state_.statusbar_popover_load_visible = false;
  state_.statusbar_popover_circuit_load.clear();
  state_.statusbar_popover_media_sessions.clear();
  state_.statusbar_popover_media_participants.clear();
  state_.statusbar_popover_network_visible = false;
  state_.statusbar_popover_network_links.clear();
  state_.statusbar_popover_network_parked.clear();
  state_.statusbar_popover_network_rate.clear();
  state_.statusbar_popover_network_rtt.clear();
  state_.statusbar_popover_network_resend.clear();
  state_.statusbar_popover_relay_rate.clear();
}

bool ShellStatusbar::ApplyPopover(const StatusbarPopoverSnapshot& snap) {
  const ui::String brief = snap.brief_label.c_str();
  const ui::String direct = snap.direct_label.c_str();
  const ui::String reach = snap.reachability_status_label.c_str();
  const ui::String summary = snap.reachability_summary.c_str();
  const ui::String help = snap.help_label.c_str();
  const ui::String upnp = snap.upnp_label.c_str();
  const ui::String error = snap.last_error.c_str();
  const bool error_visible = !snap.last_error.empty();
  const ui::String circuit_load = snap.circuit_load_label.c_str();
  const ui::String media_sessions = snap.media_sessions_label.c_str();
  const ui::String media_participants = snap.media_participants_label.c_str();
  const ui::String network_links = snap.network_links_label.c_str();
  const ui::String network_parked = snap.network_parked_label.c_str();
  const ui::String network_rate = snap.network_rate_label.c_str();
  const ui::String network_rtt = snap.network_rtt_label.c_str();
  const ui::String network_resend = snap.network_resend_label.c_str();
  const ui::String relay_rate = snap.relay_rate_label.c_str();

  if (state_.statusbar_popover_brief_label == brief && state_.statusbar_popover_direct_label == direct &&
      state_.statusbar_popover_reach_label == reach && state_.statusbar_popover_reach_summary == summary &&
      state_.statusbar_popover_help_visible == snap.help_visible &&
      state_.statusbar_popover_help_label == help &&
      state_.statusbar_popover_upnp_visible == snap.show_upnp &&
      state_.statusbar_popover_upnp_label == upnp &&
      state_.statusbar_popover_error_visible == error_visible &&
      state_.statusbar_popover_error == error &&
      state_.statusbar_popover_load_visible == snap.show_load &&
      state_.statusbar_popover_circuit_load == circuit_load &&
      state_.statusbar_popover_media_sessions == media_sessions &&
      state_.statusbar_popover_media_participants == media_participants &&
      state_.statusbar_popover_network_visible == snap.show_network &&
      state_.statusbar_popover_network_links == network_links &&
      state_.statusbar_popover_network_parked == network_parked &&
      state_.statusbar_popover_network_rate == network_rate &&
      state_.statusbar_popover_network_rtt == network_rtt &&
      state_.statusbar_popover_network_resend == network_resend &&
      state_.statusbar_popover_relay_rate == relay_rate) {
    return false;
  }

  state_.statusbar_popover_brief_label = brief;
  state_.statusbar_popover_direct_label = direct;
  state_.statusbar_popover_reach_label = reach;
  state_.statusbar_popover_reach_summary = summary;
  state_.statusbar_popover_help_visible = snap.help_visible;
  state_.statusbar_popover_help_label = help;
  state_.statusbar_popover_upnp_visible = snap.show_upnp;
  state_.statusbar_popover_upnp_label = upnp;
  state_.statusbar_popover_error_visible = error_visible;
  state_.statusbar_popover_error = error;
  state_.statusbar_popover_load_visible = snap.show_load;
  state_.statusbar_popover_circuit_load = circuit_load;
  state_.statusbar_popover_media_sessions = media_sessions;
  state_.statusbar_popover_media_participants = media_participants;
  state_.statusbar_popover_network_visible = snap.show_network;
  state_.statusbar_popover_network_links = network_links;
  state_.statusbar_popover_network_parked = network_parked;
  state_.statusbar_popover_network_rate = network_rate;
  state_.statusbar_popover_network_rtt = network_rtt;
  state_.statusbar_popover_network_resend = network_resend;
  state_.statusbar_popover_relay_rate = relay_rate;
  return true;
}

void ShellStatusbar::RefreshPopover() {
  if (!state_.statusbar_popover_open) {
    return;
  }
  StatusbarPopoverSnapshot snap;
  if (ports_.statusbar_popover) {
    snap = BuildStatusbarPopoverSnapshot(ports_.statusbar_popover());
  }
  if (ApplyPopover(snap)) {
    DirtyPopover();
  }
}

void ShellStatusbar::OpenPopover() {
  if (!state_.statusbar_visible) {
    return;
  }
  if (state_.statusbar_popover_open) {
    return;
  }
  ContextMenuHost::Instance().Dismiss();
  state_.statusbar_popover_open = true;
  popover_needs_position_ = true;
  RefreshPopover();
  DirtyPopover();
}

void ShellStatusbar::ClosePopover() {
  if (!state_.statusbar_popover_open) {
    return;
  }
  state_.statusbar_popover_open = false;
  popover_needs_position_ = false;
  ClearPopover();
  DirtyPopover();
}

void ShellStatusbar::TogglePopover() {
  if (state_.statusbar_popover_open) {
    ClosePopover();
  } else {
    OpenPopover();
  }
}

void ShellStatusbar::PositionPopover() {
  if (!state_.statusbar_popover_open || !context_ || context_->GetNumDocuments() == 0) {
    return;
  }
  ui::ElementDocument* doc = context_->GetDocument(0);
  if (!doc) {
    return;
  }
  ui::Element* cluster = doc->GetElementById("shell-statusbar-cluster");
  ui::Element* panel = doc->GetElementById("shell-statusbar-popover");
  if (!cluster || !panel) {
    return;
  }

  doc->UpdateDocument();
  const ui::Vector2i dims = context_->GetDimensions();
  const ui::Vector2f cluster_offset = cluster->GetAbsoluteOffset(ui::BoxArea::Border);
  const ui::Vector2f cluster_size = cluster->GetBox().GetSize(ui::BoxArea::Border);
  const ui::Vector2f panel_size = panel->GetBox().GetSize(ui::BoxArea::Border);
  if (panel_size.x <= 0.f || panel_size.y <= 0.f || dims.x <= 0 || dims.y <= 0) {
    return;
  }

  constexpr float kMarginPx = 8.f;
  constexpr float kGapPx = 8.f;
  float left = cluster_offset.x;
  float top = cluster_offset.y - panel_size.y - kGapPx;
  const float max_left = static_cast<float>(dims.x) - panel_size.x - kMarginPx;
  if (left > max_left) {
    left = max_left;
  }
  if (left < kMarginPx) {
    left = kMarginPx;
  }
  if (top < kMarginPx) {
    top = cluster_offset.y + cluster_size.y + kGapPx;
  }
  const float max_top = static_cast<float>(dims.y) - panel_size.y - kMarginPx;
  if (top > max_top) {
    top = std::max(kMarginPx, max_top);
  }

  panel->SetProperty("left", (std::to_string(static_cast<int>(left)) + "px").c_str());
  panel->SetProperty("top", (std::to_string(static_cast<int>(top)) + "px").c_str());
  panel->SetProperty("bottom", "auto");
  popover_needs_position_ = false;
}

void ShellStatusbar::RefreshCluster() {
  if (!state_.statusbar_visible) {
    const bool had_cluster = state_.statusbar_brief_visible || state_.statusbar_direct_visible ||
                             state_.statusbar_help_visible || state_.statusbar_inbound_visible ||
                             state_.statusbar_load_circuit_visible ||
                             state_.statusbar_load_media_visible || !state_.statusbar_label.empty();
    if (had_cluster) {
      ClearCluster();
      Dirty();
    }
    if (state_.statusbar_popover_open) {
      ClosePopover();
    }
    return;
  }
  StatusbarClusterSnapshot snap;
  if (ports_.statusbar_cluster) {
    snap = BuildStatusbarClusterSnapshot(ports_.statusbar_cluster());
  }
  if (ApplyCluster(snap)) {
    Dirty();
  }
  RefreshPopover();
  if (popover_needs_position_) {
    PositionPopover();
  }
}

void ShellStatusbar::RetestReachability() {
  if (ports_.retest_reachability) {
    ports_.retest_reachability();
  }
  RefreshPopover();
  RefreshCluster();
}

void ShellStatusbar::OpenNetworkSettings() {
  ClosePopover();
  if (ports_.open_network_settings) {
    ports_.open_network_settings();
  }
}

} // namespace pbr
