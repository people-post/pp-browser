#pragma once

#include "base/ui/ChatWidgetTypes.h"

#include <RmlUi/Core/Types.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace pbr {

class MessagingHub;

/** Thread header/banners, PSK setup, peer-link status, and sync/gap handlers. */
class ChatThreadChrome {
public:
  struct View {
    Rml::String& draft_placeholder;
    Rml::String& status;
    Rml::String& thread_title;
    Rml::String& thread_subtitle;
    Rml::String& peer_link_status;
    Rml::String& peer_link_banner;
    bool& show_peer_link;
    bool& show_peer_link_banner;
    bool& show_retry_peer_dial;
    bool& thread_encrypted;
    bool& thread_is_ai;
    bool& thread_is_private;
    bool& thread_is_public;
    bool& thread_is_group;
    bool& compose_disabled;
    bool& show_thread_actions;
    bool& show_peer_sheet;
    bool& show_call_actions;
    bool& show_forget_memory;
    bool& show_sync_with_peer;
    bool& show_thread_menu;
    bool& show_gap_banner;
    bool& show_compromised_banner;
    bool& show_psk_setup_banner;
    bool& show_psk_import;
    bool& psk_has_key;
    bool& psk_verified;
    Rml::String& psk_fingerprint;
    Rml::String& psk_export_b64;
    Rml::String& psk_import_text;
    bool& sync_in_progress;
    bool& show_older_history_hint;
    std::vector<TranscriptDisplayRow>& turns;
    std::vector<MessageDisplayRow>& messages;
    bool& use_messages_layout;
    bool& has_turns;
  };

  ChatThreadChrome(View view, bool& messaging_ready);
  void BindMessaging(MessagingHub& messaging);
  MessagingHub& Hub();
  const MessagingHub& Hub() const;

  void SetRefreshFromMessaging(std::function<void()> refresh) { refresh_ = std::move(refresh); }
  void SetWithSecrets(std::function<void(std::function<void()>)> with_secrets) {
    with_secrets_ = std::move(with_secrets);
  }
  void SetOnScrollerReset(std::function<void()> reset) { on_scroller_reset_ = std::move(reset); }
  void SetCaptureScrollBeforePrepend(std::function<void()> capture) {
    capture_scroll_before_prepend_ = std::move(capture);
  }
  void SetExpandLoadedMinFromOlderPage(std::function<void(const std::string&, int64_t)> expand) {
    expand_loaded_min_ = std::move(expand);
  }

  void Update();
  void UpdatePeerLink();
  void ResetPanelState();
  /** Poll peer-link chrome on a short interval; returns true if header fields changed. */
  bool MaybePollPeerLink(std::chrono::steady_clock::time_point now);

  void OnSyncWithPeer();
  void OnRetryGapSync();
  void OnStartNewSecureChat();
  void OnPauseIntegrityOnly();
  void OnCopyPskKey();
  void OnTogglePskImport();
  void OnImportPsk();
  void OnVerifyPsk();
  void OnRotatePskExport();
  void OnRetryPeerDial();
  void OnLoadOlderHistory();

private:
  View view_;
  bool& messaging_ready_;
  std::function<void()> refresh_;
  std::function<void(std::function<void()>)> with_secrets_;
  std::function<void()> on_scroller_reset_;
  std::function<void()> capture_scroll_before_prepend_;
  std::function<void(const std::string&, int64_t)> expand_loaded_min_;
  std::chrono::steady_clock::time_point last_peer_link_poll_{};
  MessagingHub* messaging_ = nullptr;

};

} // namespace pbr
