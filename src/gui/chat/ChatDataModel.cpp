#include "gui/chat/ChatDataModel.h"

#include "feature/conversations/ChatPresenter.h"

#include "gui/shell/DataModelHost.h"
#include "gui/chat/ChatWidgetStateBuilder.h"

namespace pbr {

void BindChatScreenState(ui::DataModelConstructor& ctor, ChatScreenState& state) {
  RegisterChatWidgetDataTypes(ctor);
  ctor.Bind("draft", &state.draft);
  ctor.Bind("draft_placeholder", &state.draft_placeholder);
  ctor.Bind("status", &state.status);
  ctor.Bind("loading", &state.loading);
  ctor.Bind("has_turns", &state.has_turns);
  ctor.Bind("turns", &state.turns);
  ctor.Bind("messages", &state.messages);
  ctor.Bind("use_messages_layout", &state.use_messages_layout);
  ctor.Bind("thread_title", &state.thread_title);
  ctor.Bind("thread_subtitle", &state.thread_subtitle);
  ctor.Bind("peer_link_status", &state.peer_link_status);
  ctor.Bind("peer_link_banner", &state.peer_link_banner);
  ctor.Bind("show_peer_link", &state.show_peer_link);
  ctor.Bind("show_peer_link_banner", &state.show_peer_link_banner);
  ctor.Bind("peer_link_direct", &state.peer_link_direct);
  ctor.Bind("peer_link_via_hop", &state.peer_link_via_hop);
  ctor.Bind("peer_link_via_relay", &state.peer_link_via_relay);
  ctor.Bind("peer_link_connecting", &state.peer_link_connecting);
  ctor.Bind("peer_link_degraded", &state.peer_link_degraded);
  ctor.Bind("peer_link_failed", &state.peer_link_failed);
  ctor.Bind("peer_link_ready", &state.peer_link_ready);
  ctor.Bind("show_retry_peer_dial", &state.show_retry_peer_dial);
  ctor.Bind("thread_encrypted", &state.thread_encrypted);
  ctor.Bind("thread_is_ai", &state.thread_is_ai);
  ctor.Bind("thread_is_private", &state.thread_is_private);
  ctor.Bind("thread_is_public", &state.thread_is_public);
  ctor.Bind("thread_is_group", &state.thread_is_group);
  ctor.Bind("compose_disabled", &state.compose_disabled);
  ctor.Bind("composer_input_disabled", &state.composer_input_disabled);
  ctor.Bind("show_attach_button", &state.show_attach_button);
  ctor.Bind("image_chip", &state.image_chip);
  ctor.Bind("image_preparing", &state.image_preparing);
  ctor.Bind("image_thumb_ready", &state.image_thumb_ready);
  ctor.Bind("image_thumb_src", &state.image_thumb_src);
  ctor.Bind("image_draft_name", &state.image_draft_name);
  ctor.Bind("quote_reply", &state.quote_reply);
  ctor.Bind("quote_reply_text", &state.quote_reply_text);
  ctor.Bind("attachment_uploading", &state.attachment_uploading);
  ctor.Bind("attachment_draft_name", &state.attachment_draft_name);
  ctor.Bind("show_thread_actions", &state.show_thread_actions);
  ctor.Bind("show_peer_sheet", &state.show_peer_sheet);
  ctor.Bind("show_call_actions", &state.show_call_actions);
  ctor.Bind("call_joinable", &state.call_joinable);
  ctor.Bind("show_forget_memory", &state.show_forget_memory);
  ctor.Bind("show_sync_with_peer", &state.show_sync_with_peer);
  ctor.Bind("show_thread_menu", &state.show_thread_menu);
  ctor.Bind("show_gap_banner", &state.show_gap_banner);
  ctor.Bind("show_compromised_banner", &state.show_compromised_banner);
  ctor.Bind("show_locked_out_banner", &state.show_locked_out_banner);
  ctor.Bind("show_psk_setup_banner", &state.show_psk_setup_banner);
  ctor.Bind("show_psk_import", &state.show_psk_import);
  ctor.Bind("psk_has_key", &state.psk_has_key);
  ctor.Bind("psk_verified", &state.psk_verified);
  ctor.Bind("psk_fingerprint", &state.psk_fingerprint);
  ctor.Bind("psk_export_b64", &state.psk_export_b64);
  ctor.Bind("psk_import_text", &state.psk_import_text);
  ctor.Bind("sync_in_progress", &state.sync_in_progress);
  ctor.Bind("show_older_history_hint", &state.show_older_history_hint);
  ctor.Bind("show_jump_to_latest", &state.show_jump_to_latest);
  ctor.Bind("jump_to_latest_label", &state.jump_to_latest_label);
}

void BindChatShellState(ui::DataModelConstructor& ctor, ChatShellState& state) {
  RegisterChatWidgetDataTypes(ctor);
  if (auto working_set_handle = ctor.RegisterStruct<TurnWidgetState>()) {
    working_set_handle.RegisterMember("has_form", &TurnWidgetState::has_form);
    working_set_handle.RegisterMember("form", &TurnWidgetState::form);
    working_set_handle.RegisterMember("has_calendar", &TurnWidgetState::has_calendar);
    working_set_handle.RegisterMember("calendar", &TurnWidgetState::calendar);
  }
  if (auto session_handle = ctor.RegisterStruct<SessionDisplayRow>()) {
    session_handle.RegisterMember("id", &SessionDisplayRow::id);
    session_handle.RegisterMember("title", &SessionDisplayRow::title);
    session_handle.RegisterMember("preview", &SessionDisplayRow::preview);
    session_handle.RegisterMember("kind", &SessionDisplayRow::kind);
    session_handle.RegisterMember("unread_count", &SessionDisplayRow::unread_count);
    session_handle.RegisterMember("unread_display", &SessionDisplayRow::unread_display);
    session_handle.RegisterMember("date_label", &SessionDisplayRow::date_label);
    session_handle.RegisterMember("active", &SessionDisplayRow::active);
    session_handle.RegisterMember("pinned", &SessionDisplayRow::pinned);
    session_handle.RegisterMember("muted", &SessionDisplayRow::muted);
    session_handle.RegisterMember("unread_dot", &SessionDisplayRow::unread_dot);
  }
  ctor.RegisterArray<std::vector<SessionDisplayRow>>();
  ctor.Bind("sessions", &state.sessions);
  ctor.Bind("archived_count", &state.archived_count);
  ctor.Bind("showing_archived", &state.showing_archived);
  ctor.Bind("archived_entry_label", &state.archived_entry_label);
  ctor.Bind("working_set_active", &state.working_set_active);
  ctor.Bind("working_set_title", &state.working_set_title);
  ctor.Bind("working_set_subtitle", &state.working_set_subtitle);
  ctor.Bind("working_set_rml", &state.working_set_rml);
  ctor.Bind("working_set", &state.working_set);
}

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
  DataModelHost::Instance().Dirty("chat", "peer_link_direct");
  DataModelHost::Instance().Dirty("chat", "peer_link_via_hop");
  DataModelHost::Instance().Dirty("chat", "peer_link_via_relay");
  DataModelHost::Instance().Dirty("chat", "peer_link_connecting");
  DataModelHost::Instance().Dirty("chat", "peer_link_degraded");
  DataModelHost::Instance().Dirty("chat", "peer_link_failed");
  DataModelHost::Instance().Dirty("chat", "peer_link_ready");
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
  DataModelHost::Instance().Dirty("chat", "image_chip");
  DataModelHost::Instance().Dirty("chat", "image_preparing");
  DataModelHost::Instance().Dirty("chat", "image_thumb_ready");
  DataModelHost::Instance().Dirty("chat", "image_thumb_src");
  DataModelHost::Instance().Dirty("chat", "image_draft_name");
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
  DataModelHost::Instance().Dirty("shell", "archived_count");
  DataModelHost::Instance().Dirty("shell", "showing_archived");
  DataModelHost::Instance().Dirty("shell", "archived_entry_label");
  DataModelHost::Instance().Dirty("shell", "working_set_active");
  DataModelHost::Instance().Dirty("shell", "working_set_title");
  DataModelHost::Instance().Dirty("shell", "working_set_subtitle");
  DataModelHost::Instance().Dirty("shell", "working_set_rml");
  DataModelHost::Instance().Dirty("shell", "working_set");
}

ui::String SessionVisualKind(const Thread& thread) {
  return ChatSessionKindName(thread).c_str();
}

} // namespace pbr
