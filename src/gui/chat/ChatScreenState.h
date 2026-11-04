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
  /** Phone header: groups only, the member count ("12 members"); empty for direct and AI chats. */
  ui::String thread_phone_subtitle;
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
  /** Phone chat title bar: the avatar of the chat list row (first letter of the title, colour index 0..7). */
  ui::String thread_avatar_letter;
  int thread_avatar_tone = 0;
  /** The person's own cached profile photo (local file), when there is one; else the letter avatar shows. */
  ui::String thread_icon_src;
  bool thread_has_icon = false;
  bool compose_disabled = false;
  bool composer_input_disabled = false;
  bool show_attach_button = false;
  bool attachment_uploading = false;
  ui::String attachment_draft_name;
  /** The chip's line while sending ("Uploading 37%") and after a failure; empty falls back to "Uploading…". */
  ui::String attachment_status;
  /** A send did not finish: the chip offers Retry / Discard (the staged ciphertext is kept). */
  bool attachment_failed = false;
  /** The camera icon in the composer pill (phones; see ShowCaptureEntry). */
  bool show_capture = false;
  /** The microphone button: this chat can take a voice message (peer or group chat, voice supported here). */
  bool show_voice = false;
  /** A call (or ringing) has the microphone: the button is grey and its tooltip says why. */
  bool voice_blocked = false;
  bool voice_recording = false;
  /** Released to send (or the limit ran out) and the file is still being written: the bar says "Sending…". */
  bool voice_finishing = false;
  /** A picked / captured file is being copied before the upload starts (iOS): the chip says "Sending…". */
  bool attachment_preparing = false;
  /** The chip's condition: an attachment is preparing / uploading / failed in the chat open now (not in another one). */
  bool attachment_here = false;
  /** Voice mode (WeChat style): a "hold to talk" bar replaces the text input. Per app session; off whenever show_voice is. */
  bool voice_mode = false;
  /** Desktop: a click starts, and Send / Cancel buttons end it. Phones: hold, release sends, slide up cancels. */
  bool voice_click_mode = false;
  /** The finger slid far enough up that releasing now cancels. */
  bool voice_cancel_zone = false;
  /** The last seconds before the limit: the timer counts down. */
  bool voice_countdown = false;
  ui::String voice_timer;
  ui::String voice_hint;
  ui::String voice_title;
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
  /** Phones: the bar under the header while that call may be joined ("" = no bar). */
  ui::String call_bar_text;
  bool show_forget_memory = false;
  bool show_sync_with_peer = false;
  bool show_thread_menu = false;
  /** The "⋯" menu offers "Search" (a chat is open and messaging is ready). */
  bool show_search = false;
  /** Search inside the open chat: the search box replaces the header, the step bar replaces the composer. */
  bool search_open = false;
  /** The first index is being built ("Searching…"). */
  bool search_indexing = false;
  ui::String search_query;
  /** "Searching…", "No results" or "3 of 12"; empty while nothing is typed. */
  ui::String search_count_label;
  bool search_has_hits = false;
  bool search_can_older = false;
  bool search_can_newer = false;
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
  bool has_queued = false;  // this thread has an AI send waiting for a free turn
  bool busy_elsewhere = false; // other chats take every turn: a question here would wait (shown above the composer)
  ui::String queued_text;   // its text, shown as a dimmed "me" bubble with "queued" and a cancel link
  ui::String queued_note;   // the line under it: why it waits ("Up to 3 chats answer at once — queued")
};

/** One headline of the phone Today page's "Today's briefs" widget. */
struct HomeBriefRow {
  ui::String title;
  /** "site · 08:00". */
  ui::String meta;
};

/** One widget drawn on the phone Home, in the user's order: wide (a row) or small (half a row). */
struct HomeWidgetSlot {
  ui::String id;
  bool wide = false;
};

/** One person in Home's Favorites widget (avatar and name; a tap opens the chat). */
struct HomeFavoriteRow {
  ui::String contact_id;
  ui::String title;
  ui::String avatar_letter;
  int avatar_tone = 0;
  bool has_icon = false;
  ui::String icon_src;
};

/** One row of the Edit Home sheet: a widget's name and its size note ("Wide" / "Small"). */
struct HomeEditRow {
  ui::String id;
  ui::String name;
  ui::String size;
};

/** The shell's chat side of its data model ("shell"): the chats list and the working set panel. */
struct ChatShellState {
  std::vector<SessionDisplayRow> sessions;
  /** Archived threads are kept out of the list; the entry row above it opens them. */
  int archived_count = 0;
  bool showing_archived = false;
  ui::String archived_entry_label;
  /** Rows remain beyond the window the sidebar draws (the "Loading…" line). */
  bool sessions_more = false;
  /** The search box above the list: rows are kept when their title or preview contains it. */
  ui::String sessions_query;
  /**
   * Phone Today page (DESIGN-pp-ui 8.16, mockup 10-home-widgets): the date line and the widgets drawn, in the user's
   * order (home_widgets), each with its own data below. A widget shows only when it has something (Favorites always).
   */
  ui::String home_date_label;
  std::vector<HomeWidgetSlot> home_widgets;
  std::vector<HomeFavoriteRow> home_favorites;
  std::vector<SessionDisplayRow> home_recent;
  int home_missed_calls = 0;
  /** "Latest: Hongwei · Video · 16:20". */
  ui::String home_calls_latest;
  int home_mentions = 0;
  /** "Hiking group · Fang: @you what time on Saturday?". */
  ui::String home_mentions_where;
  int home_new_people = 0;
  std::vector<HomeBriefRow> home_brief;
  /** Edit Home: the widgets on Home (in order) and the ones that can be added. */
  std::vector<HomeEditRow> home_edit_on;
  std::vector<HomeEditRow> home_edit_off;
  /** The widget whose handle is being dragged in Edit Home ("" = none): its row is lifted. */
  ui::String home_edit_dragging;
  bool working_set_active = false;
  ui::String working_set_title;
  ui::String working_set_subtitle;
  ui::String working_set_rml;
  TurnWidgetState working_set;
};

} // namespace pbr
