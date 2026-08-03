#include <stdexcept>
#include "feature/chat/ChatThreadChrome.h"

#include "feature/chat/ChatDataModel.h"
#include "base/messaging/SyncStateTypes.h"
#include "base/messaging/ThreadTypes.h"
#include "base/people/PeerDisplayLabel.h"
#include "feature/messaging/MessagingHub.h"

#include <RmlUi/Core/SystemInterface.h>

namespace pbr {

void ChatThreadChrome::BindMessaging(MessagingHub& messaging) {
  messaging_ = &messaging;
}

void ChatThreadChrome::BindShellNavigation(ShellNavigationPorts ports) {
  shell_navigation_ = std::move(ports);
}

void ChatThreadChrome::BindShellFeedback(ShellFeedbackPorts ports) {
  shell_feedback_ = std::move(ports);
}

namespace {

void ShellDirty(const ShellNavigationPorts& ports) {
  if (ports.dirty_window) {
    ports.dirty_window();
  }
}

void ShowToast(const ShellFeedbackPorts& ports, const std::string& message) {
  if (ports.show_toast) {
    ports.show_toast(message, ToastDuration::Short);
  }
}

void ShowConfirm(const ShellFeedbackPorts& ports, const std::string& title, const std::string& message,
                 std::function<void(bool)> on_result) {
  if (ports.show_confirm) {
    ports.show_confirm(title, message, std::move(on_result));
  }
}

void ShowConfirmWithCheckbox(const ShellFeedbackPorts& ports, const std::string& title, const std::string& message,
                             const std::string& checkbox_label, const bool checkbox_default,
                             std::function<void(bool, bool)> on_result) {
  if (ports.show_confirm_with_checkbox) {
    ports.show_confirm_with_checkbox(title, message, checkbox_label, checkbox_default, std::move(on_result));
  }
}

void ShowAlert(const ShellFeedbackPorts& ports, const std::string& title, const std::string& message,
               std::function<void()> on_ok) {
  if (ports.show_alert) {
    ports.show_alert(title, message, std::move(on_ok));
  }
}

} // namespace

MessagingHub& ChatThreadChrome::Hub() {
  if (!messaging_) {
    throw std::runtime_error("ChatThreadChrome messaging not bound");
  }
  return *messaging_;
}

const MessagingHub& ChatThreadChrome::Hub() const {
  if (!messaging_) {
    throw std::runtime_error("ChatThreadChrome messaging not bound");
  }
  return *messaging_;
}

ChatThreadChrome::ChatThreadChrome(View view, bool& messaging_ready)
    : view_(view), messaging_ready_(messaging_ready) {}

void ChatThreadChrome::ResetPanelState() {
  view_.thread_title = "";
  view_.thread_subtitle = "";
  view_.peer_link_status = "";
  view_.peer_link_banner = "";
  view_.show_peer_link = false;
  view_.show_peer_link_banner = false;
  view_.show_retry_peer_dial = false;
  view_.thread_encrypted = false;
  view_.thread_is_ai = false;
  view_.thread_is_private = false;
  view_.thread_is_public = false;
  view_.thread_is_group = false;
  view_.compose_disabled = false;
  view_.show_thread_actions = false;
  view_.show_call_actions = false;
  view_.show_forget_memory = false;
  view_.show_sync_with_peer = false;
  view_.show_thread_menu = false;
  view_.show_gap_banner = false;
  view_.show_older_history_hint = false;
  view_.show_compromised_banner = false;
  view_.show_psk_setup_banner = false;
  view_.show_psk_import = false;
  view_.psk_has_key = false;
  view_.psk_verified = false;
  view_.psk_fingerprint = "";
  view_.psk_export_b64 = "";
  view_.messages.clear();
  view_.turns.clear();
  view_.has_turns = false;
  view_.use_messages_layout = true;
  view_.draft_placeholder = "Ask anything…";
  if (on_scroller_reset_) {
    on_scroller_reset_();
  }
}

void ChatThreadChrome::UpdatePeerLink() {
  view_.peer_link_status = "";
  view_.peer_link_banner = "";
  view_.show_peer_link = false;
  view_.show_peer_link_banner = false;
  view_.show_retry_peer_dial = false;
  if (!messaging_ready_ || !Hub().IsMessagingReady()) {
    return;
  }
  auto thread = Hub().Inbox().GetActiveThread();
  if (!thread || thread->kind != ThreadKind::Direct) {
    return;
  }
  const ThreadPeerLinkView link = Hub().P2p().GetThreadPeerLink(thread->id);
  view_.show_peer_link = !link.status_label.empty();
  view_.peer_link_status = link.status_label.c_str();
  view_.show_peer_link_banner = link.show_banner && !link.banner_message.empty();
  view_.peer_link_banner = link.banner_message.c_str();
  view_.show_retry_peer_dial = link.show_retry;
}

void ChatThreadChrome::Update() {
  if (!messaging_ready_) {
    return;
  }
  if (auto thread = Hub().Inbox().GetActiveThread()) {
    const PeerDisplayLabel label = Hub().Inbox().ResolveThreadLabel(*thread);
    view_.thread_title = label.title.c_str();
    view_.thread_encrypted = thread->encrypted;
    const Rml::String visual_kind = SessionVisualKind(*thread);
    view_.thread_is_ai = visual_kind == "ai";
    view_.thread_is_private = visual_kind == "private";
    view_.thread_is_public = visual_kind == "public";
    view_.thread_is_group = visual_kind == "group";
    view_.show_peer_sheet = thread->kind == ThreadKind::Direct || thread->kind == ThreadKind::Group;
    view_.show_call_actions =
        (thread->kind == ThreadKind::Direct || thread->kind == ThreadKind::Group) && messaging_ready_;
    view_.show_thread_actions = true;
    view_.show_forget_memory = thread->kind == ThreadKind::Ai;
    view_.show_sync_with_peer = false;
    view_.show_gap_banner = false;
    view_.show_older_history_hint = false;
    view_.show_compromised_banner = false;
    view_.show_psk_setup_banner = false;
    view_.show_psk_import = false;
    view_.psk_has_key = false;
    view_.psk_verified = false;
    view_.psk_fingerprint = "";
    view_.psk_export_b64 = "";
    if (thread->kind == ThreadKind::Direct && thread->channel == ThreadChannel::E2e) {
      if (auto epoch = Hub().Store().GetChatTargetSessionEpoch(thread->id)) {
        if (auto sync_state = Hub().Store().GetPeerSyncState(thread->id, *epoch)) {
          const bool compromised = sync_state->phase == PeerSyncPhase::Compromised;
          view_.show_compromised_banner = compromised;
          view_.compose_disabled = compromised;
          if (!compromised) {
            view_.show_sync_with_peer = true;
            view_.show_gap_banner = sync_state->phase == PeerSyncPhase::Gap;
            view_.show_older_history_hint =
                sync_state->loaded_min_seq > sync_state->history_floor_seq + 1;
          }
        } else {
          view_.show_sync_with_peer = true;
        }
      } else {
        view_.show_sync_with_peer = true;
      }

      if (!view_.show_compromised_banner) {
        if (!Hub().IsMessagingReady()) {
          view_.show_psk_setup_banner = true;
          view_.compose_disabled = true;
        } else if (auto status = Hub().P2p().GetPskStatus(thread->id)) {
          view_.psk_has_key = status->has_psk;
          view_.psk_verified = status->verified;
          view_.psk_fingerprint = status->fingerprint.c_str();
          view_.show_psk_setup_banner = !status->has_psk || !status->verified;
          if (status->has_psk) {
            if (auto exported = Hub().P2p().GetPskExportView(thread->id)) {
              view_.psk_export_b64 = exported->master_psk_b64.c_str();
            }
          } else if (auto generated = Hub().P2p().EnsurePskGenerated(thread->id)) {
            view_.psk_has_key = true;
            view_.psk_fingerprint = generated->fingerprint.c_str();
            view_.psk_export_b64 = generated->master_psk_b64.c_str();
            view_.show_psk_setup_banner = true;
          }
          view_.compose_disabled = !status->has_psk || !status->verified;
        }
      }
    } else if (thread->kind == ThreadKind::Direct && thread->channel == ThreadChannel::E2ePublic) {
      view_.compose_disabled = !Hub().IsMessagingReady();
    } else if (thread->kind == ThreadKind::Group) {
      view_.compose_disabled = !Hub().IsMessagingReady();
    }
    if (thread->kind == ThreadKind::Ai) {
      view_.thread_subtitle = "Local assistant";
      view_.draft_placeholder = "Ask anything…";
    } else if (thread->kind == ThreadKind::Direct) {
      if (thread->channel == ThreadChannel::E2ePublic) {
        view_.thread_subtitle = "Encrypted · easy start";
        view_.draft_placeholder = "Message… · @ai · @ai+ · @ai++";
      } else if (thread->channel == ThreadChannel::E2e) {
        view_.thread_subtitle = "Verified private · E2E";
        view_.draft_placeholder = "Secure message… · @ai · @ai+ · @ai++";
      } else {
        view_.thread_subtitle = "Direct message";
        view_.draft_placeholder = "Message… · @ai · @ai+ · @ai++";
      }
      if (label.trust == PeerLabelTrust::DirectoryUnverified) {
        view_.thread_subtitle = std::string(view_.thread_subtitle.c_str()) + " · Unverified";
      }
    } else {
      std::string roster_label = thread->encrypted ? "Group · E2E" : "Group chat";
      if (Hub().IsMessagingReady() && thread->group_id) {
        if (auto roster = Hub().Groups().ListRoster(*thread->group_id)) {
          roster_label += " · " + std::to_string(roster->size()) + " members";
        }
        if (Hub().Groups().IsOwnerUnreachable(*thread->group_id)) {
          roster_label += " · Owner unreachable";
        }
      }
      if (label.shared_title) {
        roster_label = "Shared: " + *label.shared_title + " · " + roster_label;
      }
      view_.thread_subtitle = roster_label.c_str();
      view_.draft_placeholder = "Message the group… or @ai ask assistant";
    }
    view_.show_thread_menu =
        view_.show_thread_actions || view_.show_forget_memory || view_.show_sync_with_peer;
    UpdatePeerLink();
  } else {
    view_.thread_title = "";
    view_.thread_subtitle = "";
    view_.show_peer_sheet = false;
    view_.peer_link_status = "";
    view_.peer_link_banner = "";
    view_.show_peer_link = false;
    view_.show_peer_link_banner = false;
    view_.show_retry_peer_dial = false;
    view_.thread_encrypted = false;
    view_.thread_is_ai = false;
    view_.thread_is_private = false;
    view_.thread_is_public = false;
    view_.thread_is_group = false;
    view_.compose_disabled = false;
    view_.show_thread_actions = false;
    view_.show_peer_sheet = false;
    view_.show_call_actions = false;
    view_.show_forget_memory = false;
    view_.show_sync_with_peer = false;
    view_.show_thread_menu = false;
    view_.show_gap_banner = false;
    view_.show_older_history_hint = false;
    view_.show_compromised_banner = false;
    view_.show_psk_setup_banner = false;
    view_.show_psk_import = false;
    view_.psk_has_key = false;
    view_.psk_verified = false;
    view_.psk_fingerprint = "";
    view_.psk_export_b64 = "";
    view_.draft_placeholder = "Ask anything…";
  }
}

bool ChatThreadChrome::MaybePollPeerLink(const std::chrono::steady_clock::time_point now) {
  if (now - last_peer_link_poll_ < std::chrono::milliseconds(400)) {
    return false;
  }
  last_peer_link_poll_ = now;
  if (!messaging_ready_ || !Hub().IsMessagingReady()) {
    return false;
  }
  auto thread = Hub().Inbox().GetActiveThread();
  if (!thread || thread->kind != ThreadKind::Direct) {
    return false;
  }
  const Rml::String prev_status = view_.peer_link_status;
  const Rml::String prev_banner = view_.peer_link_banner;
  const bool prev_show = view_.show_peer_link;
  const bool prev_banner_show = view_.show_peer_link_banner;
  const bool prev_retry = view_.show_retry_peer_dial;
  UpdatePeerLink();
  return view_.peer_link_status != prev_status || view_.peer_link_banner != prev_banner ||
         view_.show_peer_link != prev_show || view_.show_peer_link_banner != prev_banner_show ||
         view_.show_retry_peer_dial != prev_retry;
}

void ChatThreadChrome::OnRetryPeerDial() {
  if (!messaging_ready_ || !Hub().IsMessagingReady()) {
    return;
  }
  const std::string thread_id = Hub().Inbox().ActiveThreadId();
  if (thread_id.empty()) {
    return;
  }
  Hub().P2p().RetryPeerDial(thread_id);
  UpdatePeerLink();
  DirtyChatHeader();
}

void ChatThreadChrome::OnLoadOlderHistory() {
  if (!messaging_ready_ || view_.sync_in_progress) {
    return;
  }
  const std::string thread_id = Hub().Inbox().ActiveThreadId();
  if (thread_id.empty()) {
    return;
  }

  view_.sync_in_progress = true;
  view_.status = "Loading older messages…";
  DirtyChatChrome();

  Hub().P2p().ScrollBackfill(thread_id, [this, thread_id](Roe<ChatSyncResult> result) {
    view_.sync_in_progress = false;
    view_.status = "";
    if (!result) {
      view_.status = result.error().message.c_str();
    } else if (result->ingested == 0) {
      view_.show_older_history_hint = false;
    } else if (!view_.messages.empty()) {
      if (capture_scroll_before_prepend_) {
        capture_scroll_before_prepend_();
      }
      const int64_t before = view_.messages.front().display_order;
      if (expand_loaded_min_) {
        expand_loaded_min_(thread_id, before);
      }
    }
    if (refresh_) {
      refresh_();
    }
    DirtyChatChrome();
  });
}

void ChatThreadChrome::OnSyncWithPeer() {
  if (!messaging_ready_ || view_.sync_in_progress) {
    return;
  }
  const std::string thread_id = Hub().Inbox().ActiveThreadId();
  if (thread_id.empty()) {
    return;
  }

  view_.sync_in_progress = true;
  view_.status = "Syncing missing messages from peer…";
  DirtyChatChrome();

  Hub().P2p().SyncWithPeer(thread_id, [this](Roe<ChatSyncResult> result) {
    view_.sync_in_progress = false;
    if (result) {
      view_.status = result->ingested > 0 ? "Sync complete." : "Up to date with peer.";
    } else {
      view_.status = result.error().message.c_str();
    }
    if (refresh_) {
      refresh_();
    }
    DirtyChatChrome();
    ShellDirty(shell_navigation_);
  });
}

void ChatThreadChrome::OnRetryGapSync() {
  if (!messaging_ready_ || view_.sync_in_progress) {
    return;
  }
  const std::string thread_id = Hub().Inbox().ActiveThreadId();
  if (thread_id.empty()) {
    return;
  }

  view_.sync_in_progress = true;
  view_.status = "Retrying sync for missing messages…";
  DirtyChatChrome();

  Hub().P2p().RetryGapSync(thread_id, [this](Roe<ChatSyncResult> result) {
    view_.sync_in_progress = false;
    if (result) {
      if (result->ingested > 0 || result->empty_gap_closed) {
        view_.status = "Gap repair complete.";
      } else {
        view_.status = "No missing messages found for this gap.";
      }
    } else {
      view_.status = result.error().message.c_str();
    }
    if (refresh_) {
      refresh_();
    }
    DirtyChatChrome();
    ShellDirty(shell_navigation_);
  });
}

void ChatThreadChrome::OnStartNewSecureChat() {
  if (!messaging_ready_ || !with_secrets_) {
    return;
  }
  with_secrets_([this]() {
    const std::string thread_id = Hub().Inbox().ActiveThreadId();
    if (thread_id.empty()) {
      return;
    }

    ShellFeedback::ShowConfirm(
        "Start new secure chat?",
        "This bumps the session epoch and cancels unsent messages from the previous epoch. "
        "Your saved transcript stays on this device.",
        [this, thread_id](bool ok) {
          if (!ok) {
            return;
          }
          auto result = Hub().P2p().StartNewSecureChat(thread_id);
          if (!result) {
            view_.status = result.error().message.c_str();
          } else {
            view_.status = "New secure session started.";
          }
          if (refresh_) {
            refresh_();
          }
          DirtyChatChrome();
          ShellDirty(shell_navigation_);
        });
  });
}

void ChatThreadChrome::OnPauseIntegrityOnly() {
  if (!messaging_ready_) {
    return;
  }
  const std::string thread_id = Hub().Inbox().ActiveThreadId();
  if (thread_id.empty()) {
    return;
  }

  if (!Hub().P2p().PauseIntegrityOnly(thread_id)) {
    return;
  }
  view_.status = "Messaging paused until you rotate the encryption key.";
  if (refresh_) {
    refresh_();
  }
  DirtyChatHeader();
  ShellDirty(shell_navigation_);
}

void ChatThreadChrome::OnCopyPskKey() {
  if (!messaging_ready_ || !with_secrets_) {
    return;
  }
  with_secrets_([this]() {
    const std::string thread_id = Hub().Inbox().ActiveThreadId();
    if (thread_id.empty()) {
      return;
    }
    auto exported = Hub().P2p().EnsurePskGenerated(thread_id);
    if (!exported) {
      view_.status = exported.error().message.c_str();
      DirtyChatChrome();
      return;
    }
    view_.psk_export_b64 = exported->master_psk_b64.c_str();
    view_.psk_fingerprint = exported->fingerprint.c_str();
    if (Rml::SystemInterface* system = Rml::GetSystemInterface()) {
      system->SetClipboardText(view_.psk_export_b64);
    }
    view_.status = "Encryption key copied.";
    DirtyChatHeader();
    ShellDirty(shell_navigation_);
  });
}

void ChatThreadChrome::OnTogglePskImport() {
  view_.show_psk_import = !view_.show_psk_import;
  DirtyChatHeader();
  ShellDirty(shell_navigation_);
}

void ChatThreadChrome::OnImportPsk() {
  if (!messaging_ready_) {
    return;
  }
  if (!Hub().IsMessagingReady()) {
    if (with_secrets_) {
      with_secrets_([this]() { OnImportPsk(); });
    }
    return;
  }
  const std::string thread_id = Hub().Inbox().ActiveThreadId();
  if (thread_id.empty()) {
    return;
  }
  const std::string pasted = view_.psk_import_text.c_str();
  if (pasted.empty()) {
    view_.status = "Paste a key or bundle first.";
    DirtyChatChrome();
    return;
  }

  if (pasted.find('{') != std::string::npos) {
    if (auto imported = Hub().P2p().ImportPskBundleJson(thread_id, pasted); !imported) {
      view_.status = imported.error().message.c_str();
    } else {
      view_.psk_import_text = "";
      view_.show_psk_import = false;
      view_.status = "Encryption key installed. Verify the fingerprint before sending.";
    }
  } else if (auto imported = Hub().P2p().ImportPskRawBase64(thread_id, pasted); !imported) {
    view_.status = imported.error().message.c_str();
  } else {
    view_.psk_import_text = "";
    view_.show_psk_import = false;
    view_.status = "Encryption key installed. Verify the fingerprint before sending.";
  }
  if (refresh_) {
    refresh_();
  }
  DirtyChatHeader();
  ShellDirty(shell_navigation_);
}

void ChatThreadChrome::OnVerifyPsk() {
  if (!messaging_ready_) {
    return;
  }
  if (!Hub().IsMessagingReady()) {
    if (with_secrets_) {
      with_secrets_([this]() { OnVerifyPsk(); });
    }
    return;
  }
  const std::string thread_id = Hub().Inbox().ActiveThreadId();
  if (thread_id.empty()) {
    return;
  }

  ShellFeedback::ShowConfirmWithCheckbox(
      "Verify encryption fingerprint",
      "Only confirm after you compared this fingerprint with your contact out of band.",
      "I've verified this fingerprint with my contact", false,
      [this, thread_id](const bool confirmed, const bool checked) {
        if (!confirmed || !checked) {
          return;
        }
        auto result = Hub().P2p().MarkPskVerified(thread_id);
        if (!result) {
          view_.status = result.error().message.c_str();
        } else {
          view_.status = "Encryption key verified. You can send secure messages.";
        }
        if (refresh_) {
          refresh_();
        }
        DirtyChatHeader();
        ShellDirty(shell_navigation_);
      });
}

void ChatThreadChrome::OnRotatePskExport() {
  if (!messaging_ready_) {
    return;
  }
  if (!Hub().IsMessagingReady()) {
    if (with_secrets_) {
      with_secrets_([this]() { OnRotatePskExport(); });
    }
    return;
  }
  const std::string thread_id = Hub().Inbox().ActiveThreadId();
  if (thread_id.empty()) {
    return;
  }

  ShellFeedback::ShowConfirm(
      "Rotate encryption key?",
      "This generates a new key, bumps the session epoch, and cancels unsent messages from the previous epoch. "
      "Share the exported bundle with your contact out of band.",
      [this, thread_id](const bool ok) {
        if (!ok) {
          return;
        }
        auto bundle = Hub().P2p().RotatePskAndExportBundle(thread_id);
        if (!bundle) {
          view_.status = bundle.error().message.c_str();
          DirtyChatChrome();
          ShellDirty(shell_navigation_);
          return;
        }
        if (Rml::SystemInterface* system = Rml::GetSystemInterface()) {
          system->SetClipboardText(*bundle);
        }
        ShellFeedback::ShowAlert(
            "Rotation bundle exported",
            "The pp-browser-psk-bundle-v1 JSON was copied to your clipboard. Send it to your contact securely.");
        view_.status = "Encryption key rotated. Share the bundle with your contact.";
        if (refresh_) {
          refresh_();
        }
        DirtyChatHeader();
        ShellDirty(shell_navigation_);
      });
}

} // namespace pbr
