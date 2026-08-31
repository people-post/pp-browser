#include "feature/chat/ChatDataModel.h"

#include "feature/ui/DataModelHost.h"

namespace pbr {

void DirtyChatChrome() {
  DataModelHost::Instance().Dirty("chat", "draft");
  DataModelHost::Instance().Dirty("chat", "status");
  DataModelHost::Instance().Dirty("chat", "loading");
  DataModelHost::Instance().Dirty("chat", "has_turns");
}

void DirtyChatTurns() {
  DataModelHost::Instance().Dirty("chat", "turns");
  DataModelHost::Instance().Dirty("chat", "messages");
}

void DirtyChatHeader() {
  DataModelHost::Instance().Dirty("chat", "thread_title");
  DataModelHost::Instance().Dirty("chat", "thread_subtitle");
  DataModelHost::Instance().Dirty("chat", "peer_link_status");
  DataModelHost::Instance().Dirty("chat", "peer_link_banner");
  DataModelHost::Instance().Dirty("chat", "show_peer_link");
  DataModelHost::Instance().Dirty("chat", "show_peer_link_banner");
  DataModelHost::Instance().Dirty("chat", "show_retry_peer_dial");
  DataModelHost::Instance().Dirty("chat", "thread_encrypted");
  DataModelHost::Instance().Dirty("chat", "thread_is_ai");
  DataModelHost::Instance().Dirty("chat", "thread_is_private");
  DataModelHost::Instance().Dirty("chat", "thread_is_public");
  DataModelHost::Instance().Dirty("chat", "thread_is_group");
  DataModelHost::Instance().Dirty("chat", "compose_disabled");
  DataModelHost::Instance().Dirty("chat", "composer_input_disabled");
  DataModelHost::Instance().Dirty("chat", "show_attach_button");
  DataModelHost::Instance().Dirty("chat", "attachment_uploading");
  DataModelHost::Instance().Dirty("chat", "attachment_draft_name");
  DataModelHost::Instance().Dirty("chat", "draft_placeholder");
  DataModelHost::Instance().Dirty("chat", "show_thread_actions");
  DataModelHost::Instance().Dirty("chat", "show_peer_sheet");
  DataModelHost::Instance().Dirty("chat", "show_call_actions");
  DataModelHost::Instance().Dirty("chat", "show_forget_memory");
  DataModelHost::Instance().Dirty("chat", "show_sync_with_peer");
  DataModelHost::Instance().Dirty("chat", "show_thread_menu");
  DataModelHost::Instance().Dirty("chat", "show_gap_banner");
  DataModelHost::Instance().Dirty("chat", "show_compromised_banner");
  DataModelHost::Instance().Dirty("chat", "show_locked_out_banner");
  DataModelHost::Instance().Dirty("chat", "show_psk_setup_banner");
  DataModelHost::Instance().Dirty("chat", "show_psk_import");
  DataModelHost::Instance().Dirty("chat", "psk_has_key");
  DataModelHost::Instance().Dirty("chat", "psk_verified");
  DataModelHost::Instance().Dirty("chat", "psk_fingerprint");
  DataModelHost::Instance().Dirty("chat", "psk_export_b64");
  DataModelHost::Instance().Dirty("chat", "psk_import_text");
  DataModelHost::Instance().Dirty("chat", "show_older_history_hint");
  DataModelHost::Instance().Dirty("chat", "show_jump_to_latest");
  DataModelHost::Instance().Dirty("chat", "jump_to_latest_label");
}

void DirtyChat() {
  DirtyChatChrome();
  DirtyChatTurns();
  DirtyChatHeader();
}

void DirtyShell() {
  DataModelHost::Instance().Dirty("shell", "sessions");
  DataModelHost::Instance().Dirty("shell", "working_set_active");
  DataModelHost::Instance().Dirty("shell", "working_set_title");
  DataModelHost::Instance().Dirty("shell", "working_set_subtitle");
  DataModelHost::Instance().Dirty("shell", "working_set_rml");
  DataModelHost::Instance().Dirty("shell", "working_set");
}

Rml::String SessionVisualKind(const Thread& thread) {
  switch (thread.kind) {
  case ThreadKind::Ai:
    return "ai";
  case ThreadKind::Group:
    return "group";
  case ThreadKind::Direct:
    if (thread.channel == ThreadChannel::E2e) {
      return "private";
    }
    if (thread.channel == ThreadChannel::E2ePublic) {
      return "public";
    }
    return "public";
  }
  return "public";
}

} // namespace pbr
