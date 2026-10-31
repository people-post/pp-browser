#pragma once

#include "lib/ui/ChatWidgetTypes.h"

#include <ui/base/Types.h>

#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/** The chat screen's data model ("chat"): what the GUI binds; ChatController and its parts write it. */
struct ChatScreenState {
  ui::String draft;
  ui::String draft_placeholder;
  ui::String status;
  ui::String thread_title;
  ui::String thread_subtitle;
  ui::String peer_link_status;
  ui::String peer_link_banner;
  bool show_peer_link = false;
  bool show_peer_link_banner = false;
  bool peer_link_direct = false;
  bool peer_link_via_hop = false;
  bool peer_link_via_relay = false;
  bool peer_link_connecting = false;
  bool peer_link_degraded = false;
  bool peer_link_failed = false;
  bool peer_link_ready = false;
  bool show_retry_peer_dial = false;
  bool thread_encrypted = false;
  bool thread_is_ai = false;
  bool thread_is_private = false;
  bool thread_is_public = false;
  bool thread_is_group = false;
  bool compose_disabled = false;
  bool composer_input_disabled = false;
  bool show_attach_button = false;
  bool attachment_uploading = false;
  ui::String attachment_draft_name;
  /** The chip's line while sending ("Uploading 37%") and after a failure; empty falls back to "Uploading…". */
  ui::String attachment_status;
  /** A send did not finish: the chip offers Retry / Discard (the staged ciphertext is kept). */
  bool attachment_failed = false;
  /** AI image chip: an image is being prepared (image_preparing) or waits for the question. */
  bool image_chip = false;
  bool image_preparing = false;
  bool image_thumb_ready = false;
  ui::String image_thumb_src;
  ui::String image_draft_name;
  /** A reply is being written to a quoted message: the bar above the composer input. */
  bool quote_reply = false;
  ui::String quote_reply_text;
  bool show_thread_actions = false;
  bool show_peer_sheet = false;
  bool show_call_actions = false;
  /** A group call goes on here that this device may join: the call button reads "Join" (V053). */
  bool call_joinable = false;
  bool show_forget_memory = false;
  bool show_sync_with_peer = false;
  bool show_thread_menu = false;
  bool show_gap_banner = false;
  bool show_compromised_banner = false;
  bool show_locked_out_banner = false;
  bool show_psk_setup_banner = false;
  bool show_psk_import = false;
  bool psk_has_key = false;
  bool psk_verified = false;
  ui::String psk_fingerprint;
  ui::String psk_export_b64;
  ui::String psk_import_text;
  bool sync_in_progress = false;
  bool show_older_history_hint = false;
  bool show_jump_to_latest = false;
  ui::String jump_to_latest_label;
  std::vector<TranscriptDisplayRow> turns;
  std::vector<MessageDisplayRow> messages;
  bool use_messages_layout = true;
  bool loading = false; // a turn runs in THIS thread (thinking + stop key); other threads keep sending (B112)
  bool has_turns = false;
  bool has_queued = false;  // this thread has an AI send waiting for another thread's turn
  bool busy_elsewhere = false; // the AI is answering another conversation (shown above the composer)
  ui::String queued_text;   // its text, shown as a dimmed "me" bubble with "queued" and a cancel link
};

/** The shell's chat side of its data model ("shell"): the chats list and the working set panel. */
struct ChatShellState {
  std::vector<SessionDisplayRow> sessions;
  /** Archived threads are kept out of the list; the entry row above it opens them. */
  int archived_count = 0;
  bool showing_archived = false;
  ui::String archived_entry_label;
  bool working_set_active = false;
  ui::String working_set_title;
  ui::String working_set_subtitle;
  ui::String working_set_rml;
  TurnWidgetState working_set;
};

} // namespace pbr
