#include "gui/chat/ChatDataModel.h"

#include "feature/presenters/ChatPresenter.h"

#include "gui/shell/DataModelHost.h"
#include "gui/chat/ChatWidgetStateBuilder.h"

namespace pbr {

void BindChatScreenState(ui::DataModelConstructor& ctor, ChatScreenState& state) {
  RegisterChatWidgetDataTypes(ctor);
  ctor.Bind("draft", &state.draft);
  ctor.Bind("draft_placeholder", &state.draft_placeholder);
  ctor.Bind("status", &state.status);
  ctor.Bind("loading", &state.loading);
  ctor.Bind("has_queued", &state.has_queued);
  ctor.Bind("busy_elsewhere", &state.busy_elsewhere);
  ctor.Bind("queued_text", &state.queued_text);
  ctor.Bind("queued_note", &state.queued_note);
  ctor.Bind("has_turns", &state.has_turns);
  ctor.Bind("turns", &state.turns);
  if (auto reactor_handle = ctor.RegisterStruct<ReactorDisplayRow>()) {
    reactor_handle.RegisterMember("emoji", &ReactorDisplayRow::emoji);
    reactor_handle.RegisterMember("title", &ReactorDisplayRow::title);
    reactor_handle.RegisterMember("avatar_letter", &ReactorDisplayRow::avatar_letter);
    reactor_handle.RegisterMember("avatar_tone", &ReactorDisplayRow::avatar_tone);
    reactor_handle.RegisterMember("has_icon", &ReactorDisplayRow::has_icon);
    reactor_handle.RegisterMember("icon_src", &ReactorDisplayRow::icon_src);
    reactor_handle.RegisterMember("is_self", &ReactorDisplayRow::is_self);
  }
  ctor.RegisterArray<std::vector<ReactorDisplayRow>>();
  ctor.Bind("reactor_rows", &state.reactor_rows);
  ctor.Bind("messages", &state.messages);
  ctor.Bind("use_messages_layout", &state.use_messages_layout);
  ctor.Bind("thread_title", &state.thread_title);
  ctor.Bind("thread_subtitle", &state.thread_subtitle);
  ctor.Bind("thread_phone_subtitle", &state.thread_phone_subtitle);
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
  ctor.Bind("thread_avatar_letter", &state.thread_avatar_letter);
  ctor.Bind("thread_avatar_tone", &state.thread_avatar_tone);
  ctor.Bind("thread_icon_src", &state.thread_icon_src);
  ctor.Bind("thread_has_icon", &state.thread_has_icon);
  ctor.Bind("compose_disabled", &state.compose_disabled);
  ctor.Bind("composer_input_disabled", &state.composer_input_disabled);
  ctor.Bind("attach_disabled", &state.attach_disabled);
  ctor.Bind("show_attach_button", &state.show_attach_button);
  ctor.Bind("show_capture", &state.show_capture);
  ctor.Bind("image_chip", &state.image_chip);
  ctor.Bind("image_preparing", &state.image_preparing);
  ctor.Bind("image_thumb_ready", &state.image_thumb_ready);
  ctor.Bind("image_thumb_src", &state.image_thumb_src);
  ctor.Bind("image_draft_name", &state.image_draft_name);
  ctor.Bind("quote_reply", &state.quote_reply);
  ctor.Bind("quote_reply_text", &state.quote_reply_text);
  ctor.Bind("attachment_uploading", &state.attachment_uploading);
  ctor.Bind("attachment_draft_name", &state.attachment_draft_name);
  ctor.Bind("attachment_status", &state.attachment_status);
  ctor.Bind("attachment_failed", &state.attachment_failed);
  ctor.Bind("attachment_progress_width", &state.attachment_progress_width);
  ctor.Bind("attachment_thumb", &state.attachment_thumb);
  ctor.Bind("attachment_type_label", &state.attachment_type_label);
  ctor.Bind("attachment_cancelling", &state.attachment_cancelling);
  ctor.Bind("show_voice", &state.show_voice);
  ctor.Bind("voice_entry_off", &state.voice_entry_off);
  ctor.Bind("voice_blocked", &state.voice_blocked);
  ctor.Bind("voice_recording", &state.voice_recording);
  ctor.Bind("voice_finishing", &state.voice_finishing);
  ctor.Bind("attachment_preparing", &state.attachment_preparing);
  ctor.Bind("attachment_here", &state.attachment_here);
  ctor.Bind("attachment_in_transcript", &state.attachment_in_transcript);
  ctor.Bind("voice_mode", &state.voice_mode);
  ctor.Bind("voice_click_mode", &state.voice_click_mode);
  ctor.Bind("voice_cancel_zone", &state.voice_cancel_zone);
  ctor.Bind("voice_countdown", &state.voice_countdown);
  ctor.Bind("voice_timer", &state.voice_timer);
  ctor.Bind("voice_hint", &state.voice_hint);
  ctor.Bind("voice_title", &state.voice_title);
  ctor.Bind("show_thread_actions", &state.show_thread_actions);
  ctor.Bind("show_peer_sheet", &state.show_peer_sheet);
  ctor.Bind("show_call_actions", &state.show_call_actions);
  ctor.Bind("call_joinable", &state.call_joinable);
  ctor.Bind("call_bar_text", &state.call_bar_text);
  ctor.Bind("show_forget_memory", &state.show_forget_memory);
  ctor.Bind("show_sync_with_peer", &state.show_sync_with_peer);
  ctor.Bind("show_thread_menu", &state.show_thread_menu);
  ctor.Bind("show_search", &state.show_search);
  ctor.Bind("search_open", &state.search_open);
  ctor.Bind("search_indexing", &state.search_indexing);
  ctor.Bind("search_query", &state.search_query);
  ctor.Bind("search_count_label", &state.search_count_label);
  ctor.Bind("search_has_hits", &state.search_has_hits);
  ctor.Bind("search_can_older", &state.search_can_older);
  ctor.Bind("search_can_newer", &state.search_can_newer);
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
  ctor.Bind("jump_to_latest_count", &state.jump_to_latest_count);
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
    session_handle.RegisterMember("preview_empty", &SessionDisplayRow::preview_empty);
    session_handle.RegisterMember("kind", &SessionDisplayRow::kind);
    session_handle.RegisterMember("unread_count", &SessionDisplayRow::unread_count);
    session_handle.RegisterMember("unread_display", &SessionDisplayRow::unread_display);
    session_handle.RegisterMember("date_label", &SessionDisplayRow::date_label);
    session_handle.RegisterMember("active", &SessionDisplayRow::active);
    session_handle.RegisterMember("pinned", &SessionDisplayRow::pinned);
    session_handle.RegisterMember("muted", &SessionDisplayRow::muted);
    session_handle.RegisterMember("blocked", &SessionDisplayRow::blocked);
    session_handle.RegisterMember("unread_dot", &SessionDisplayRow::unread_dot);
    session_handle.RegisterMember("mentioned", &SessionDisplayRow::mentioned);
    session_handle.RegisterMember("avatar_letter", &SessionDisplayRow::avatar_letter);
    session_handle.RegisterMember("avatar_tone", &SessionDisplayRow::avatar_tone);
    session_handle.RegisterMember("has_icon", &SessionDisplayRow::has_icon);
    session_handle.RegisterMember("icon_src", &SessionDisplayRow::icon_src);
  }
  ctor.RegisterArray<std::vector<SessionDisplayRow>>();
  if (auto brief_handle = ctor.RegisterStruct<HomeBriefRow>()) {
    brief_handle.RegisterMember("title", &HomeBriefRow::title);
    brief_handle.RegisterMember("meta", &HomeBriefRow::meta);
  }
  ctor.RegisterArray<std::vector<HomeBriefRow>>();
  if (auto slot_handle = ctor.RegisterStruct<HomeWidgetSlot>()) {
    slot_handle.RegisterMember("id", &HomeWidgetSlot::id);
    slot_handle.RegisterMember("wide", &HomeWidgetSlot::wide);
  }
  ctor.RegisterArray<std::vector<HomeWidgetSlot>>();
  if (auto favorite_handle = ctor.RegisterStruct<HomeFavoriteRow>()) {
    favorite_handle.RegisterMember("contact_id", &HomeFavoriteRow::contact_id);
    favorite_handle.RegisterMember("title", &HomeFavoriteRow::title);
    favorite_handle.RegisterMember("avatar_letter", &HomeFavoriteRow::avatar_letter);
    favorite_handle.RegisterMember("avatar_tone", &HomeFavoriteRow::avatar_tone);
    favorite_handle.RegisterMember("has_icon", &HomeFavoriteRow::has_icon);
    favorite_handle.RegisterMember("icon_src", &HomeFavoriteRow::icon_src);
    favorite_handle.RegisterMember("suggested", &HomeFavoriteRow::suggested);
  }
  ctor.RegisterArray<std::vector<HomeFavoriteRow>>();
  if (auto edit_handle = ctor.RegisterStruct<HomeEditRow>()) {
    edit_handle.RegisterMember("id", &HomeEditRow::id);
    edit_handle.RegisterMember("name", &HomeEditRow::name);
    edit_handle.RegisterMember("size", &HomeEditRow::size);
  }
  ctor.RegisterArray<std::vector<HomeEditRow>>();
  ctor.Bind("sessions", &state.sessions);
  ctor.Bind("ai_sessions", &state.ai_sessions);
  ctor.Bind("home_date_label", &state.home_date_label);
  ctor.Bind("home_widgets", &state.home_widgets);
  ctor.Bind("home_favorites", &state.home_favorites);
  ctor.Bind("home_fav_avatar", &state.home_fav_avatar);
  ctor.Bind("home_fav_avatar_radius", &state.home_fav_avatar_radius);
  ctor.Bind("home_recent", &state.home_recent);
  ctor.Bind("home_missed_calls", &state.home_missed_calls);
  ctor.Bind("home_calls_latest", &state.home_calls_latest);
  ctor.Bind("home_mentions", &state.home_mentions);
  ctor.Bind("home_mentions_where", &state.home_mentions_where);
  ctor.Bind("home_new_people", &state.home_new_people);
  ctor.Bind("home_security_ok", &state.home_security_ok);
  ctor.Bind("home_show_dynamic", &state.home_show_dynamic);
  ctor.Bind("home_brief", &state.home_brief);
  ctor.Bind("home_edit_on", &state.home_edit_on);
  ctor.Bind("home_edit_off", &state.home_edit_off);
  ctor.Bind("home_edit_dragging", &state.home_edit_dragging);
  ctor.Bind("archived_count", &state.archived_count);
  ctor.Bind("showing_archived", &state.showing_archived);
  ctor.Bind("archived_count_ai", &state.archived_count_ai);
  ctor.Bind("showing_archived_ai", &state.showing_archived_ai);
  ctor.Bind("archived_entry_label_ai", &state.archived_entry_label_ai);
  ctor.Bind("archived_entry_label", &state.archived_entry_label);
  ctor.Bind("sessions_more", &state.sessions_more);
  ctor.Bind("sessions_query", &state.sessions_query);
  ctor.Bind("ai_sessions_query", &state.ai_sessions_query);
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
  DataModelHost::Instance().Dirty("chat", "has_queued");
  DataModelHost::Instance().Dirty("chat", "busy_elsewhere");
  DataModelHost::Instance().Dirty("chat", "queued_text");
  DataModelHost::Instance().Dirty("chat", "queued_note");
  DataModelHost::Instance().Dirty("chat", "has_turns");
}

void DirtyChatTurns() {
  DataModelHost::Instance().Dirty("chat", "turns");
  DataModelHost::Instance().Dirty("chat", "messages");
}

void DirtyChatHeader() {
  DataModelHost::Instance().Dirty("chat", "thread_title");
  DataModelHost::Instance().Dirty("chat", "thread_subtitle");
  DataModelHost::Instance().Dirty("chat", "thread_phone_subtitle");
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
  DataModelHost::Instance().Dirty("chat", "thread_avatar_letter");
  DataModelHost::Instance().Dirty("chat", "thread_avatar_tone");
  DataModelHost::Instance().Dirty("chat", "thread_icon_src");
  DataModelHost::Instance().Dirty("chat", "thread_has_icon");
  DataModelHost::Instance().Dirty("chat", "compose_disabled");
  DataModelHost::Instance().Dirty("chat", "composer_input_disabled");
  DataModelHost::Instance().Dirty("chat", "attach_disabled");
  DataModelHost::Instance().Dirty("chat", "show_attach_button");
  DataModelHost::Instance().Dirty("chat", "show_capture");
  DataModelHost::Instance().Dirty("chat", "attachment_uploading");
  DataModelHost::Instance().Dirty("chat", "attachment_draft_name");
  DataModelHost::Instance().Dirty("chat", "attachment_status");
  DataModelHost::Instance().Dirty("chat", "attachment_failed");
  DataModelHost::Instance().Dirty("chat", "attachment_progress_width");
  DataModelHost::Instance().Dirty("chat", "attachment_thumb");
  DataModelHost::Instance().Dirty("chat", "attachment_type_label");
  DataModelHost::Instance().Dirty("chat", "attachment_cancelling");
  DataModelHost::Instance().Dirty("chat", "show_voice");
  DataModelHost::Instance().Dirty("chat", "voice_entry_off");
  DataModelHost::Instance().Dirty("chat", "voice_blocked");
  DataModelHost::Instance().Dirty("chat", "voice_recording");
  DataModelHost::Instance().Dirty("chat", "voice_finishing");
  DataModelHost::Instance().Dirty("chat", "attachment_preparing");
  DataModelHost::Instance().Dirty("chat", "attachment_here");
  DataModelHost::Instance().Dirty("chat", "attachment_in_transcript");
  DataModelHost::Instance().Dirty("chat", "voice_mode");
  DataModelHost::Instance().Dirty("chat", "voice_click_mode");
  DataModelHost::Instance().Dirty("chat", "voice_cancel_zone");
  DataModelHost::Instance().Dirty("chat", "voice_countdown");
  DataModelHost::Instance().Dirty("chat", "voice_timer");
  DataModelHost::Instance().Dirty("chat", "voice_hint");
  DataModelHost::Instance().Dirty("chat", "voice_title");
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
  DataModelHost::Instance().Dirty("chat", "show_search");
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
  DataModelHost::Instance().Dirty("chat", "jump_to_latest_count");
}

void DirtyChat() {
  DirtyChatChrome();
  DirtyChatTurns();
  DirtyChatHeader();
}

void DirtyShell() {
  DataModelHost::Instance().Dirty("shell", "sessions");
  DataModelHost::Instance().Dirty("shell", "ai_sessions");
  DataModelHost::Instance().Dirty("shell", "home_date_label");
  for (const char* name :
       {"home_widgets", "home_favorites", "home_recent", "home_missed_calls", "home_calls_latest", "home_mentions",
        "home_mentions_where", "home_new_people", "home_security_ok", "home_show_dynamic", "home_brief", "home_edit_on",
        "home_edit_off", "home_edit_dragging", "home_fav_avatar", "home_fav_avatar_radius"}) {
    DataModelHost::Instance().Dirty("shell", name);
  }
  DataModelHost::Instance().Dirty("shell", "archived_count");
  DataModelHost::Instance().Dirty("shell", "showing_archived");
  DataModelHost::Instance().Dirty("shell", "archived_count_ai");
  DataModelHost::Instance().Dirty("shell", "showing_archived_ai");
  DataModelHost::Instance().Dirty("shell", "archived_entry_label_ai");
  DataModelHost::Instance().Dirty("shell", "archived_entry_label");
  DataModelHost::Instance().Dirty("shell", "sessions_more");
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
