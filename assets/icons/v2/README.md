# Icon set v2 (for the UI restyle, unused so far)

White single-colour sources on a 24x24 canvas; the engine tints them with RCSS `image-color`. Stroke icons use stroke-width 1.75 with round caps/joins (DESIGN-pp-ui.md 11.4); filled variants carry the suffix `-fill`.
All shapes were drawn for the mockups in DEV/ui-mockups (not copied from Lucide or SF Symbols), so no third-party licence applies. Nothing in the app references these files yet; the old icons in `assets/icons/` stay in use until each screen is restyled.

Old icons with no v2 counterpart (not in any mockup): more, pin, play/stop/pause (pause, stop), send, settings, share, sync, trash, video-off, keyboard, archive, chevron-up, chevrons-left/right, sessions, status-*, window-*. Keep using the old ones there.

| name | used on (mockup pages; component section in DESIGN-pp-ui.md) | source | replaces old icon |
|---|---|---|---|
| at-sign | 03-home-edit | drawn | - |
| back | 01-chat, 04-contact-detail, 05-WideChats, 05-WideContacts (+18 more); §8.1 | drawn | `back.svg` |
| bell | 07-Me | drawn | - |
| bell-off | 02-chatlist, 05-WideChats, 06-GroupInfo, 06-GroupLeave (+1 more); §8.3 | drawn | `bell-off.svg` |
| bubble-tail-left | phone chat window, tail of the last received bubble of a run | drawn | - |
| bubble-tail-right | phone chat window, tail of the last sent bubble of a run | drawn | - |
| camera | 03-home, 05-Wide, 07-SetPrivacyHelp; §8.5 | drawn | `camera.svg` |
| camera-flip | 06-CallGroupVideo, 06-CallVideo; §8.12 | drawn | `camera-flip.svg` |
| chevron-down | 06-CallGroupVideo, 06-CallGroupVoice, 06-CallVideo, 06-CallVoice | drawn | `chevron-down.svg` |
| chevron-right | 04-contact-detail, 04-contacts, 04-contacts-add, 05-WideContacts (+14 more); §8.7 | drawn | `chevron-right.svg` |
| close | 07-SetPrivacyHelp; §8.9 | drawn | `close.svg` |
| compose | 02-chatlist, 05-WideChats, WideChats; §8.1 | drawn | - |
| copy | 04-contact-detail, 05-WideContacts, 07-SetProfile, WideContacts | drawn | `copy.svg` |
| database | 07-Me | drawn | - |
| delete-left | PIN gate number pad (backspace key), DEV/PLAN-pin-gate-redesign.md | drawn | - |
| face-id | PIN gate number pad (face unlock key, Face ID devices), DEV/PLAN-pin-gate-redesign.md C | drawn | - |
| fingerprint | PIN gate number pad (fingerprint unlock key, Touch ID / fingerprint devices), DEV/PLAN-pin-gate-redesign.md C | drawn | - |
| emoji | 01-chat, 05-WideChats, 06-Group, 06-GroupLive (+1 more); §8.5 | drawn | `emoji.svg` |
| globe | 07-Me | drawn | - |
| group | 02-chatlist, 03-home, 04-contacts, 04-contacts-add (+12 more); §8.2 / 8.3 / 8.13 | drawn | `contacts.svg`, `group.svg` |
| help-circle | 07-Me | drawn | - |
| info | 07-Me | drawn | - |
| lock | 01-chat, 02-chatlist, 05-WideChats, 06-CallGroupVideo (+5 more); §8.3 | drawn | `lock.svg` |
| menu | 03-home-edit | drawn | - |
| mic | 01-chat, 03-home, 05-Wide, 05-WideChats (+9 more); §8.5 / 8.12 | drawn | `mic.svg` |
| mic-off | 06-CallGroupVideo, 06-CallGroupVoice; §8.12 | drawn | `mic-off.svg` |
| minus | 03-home-edit | drawn | - |
| paperclip | 07-SetPrivacyHelp; §8.5 | drawn | `attach.svg` |
| phone | 01-chat, 03-home-edit, 04-contact-detail, 05-WideChats (+9 more); §8.12 / 8.6 | drawn | `phone.svg` |
| phone-hangup-fill | 06-CallGroupRing, 06-CallGroupVideo, 06-CallGroupVoice, 06-CallRing (+2 more); §8.12 | drawn | `phone-hangup.svg` |
| phone-declined | chat transcript call record + chat list preview (declined / did not connect) | drawn | - |
| phone-incoming | chat transcript call record + chat list preview (call you received) | drawn | - |
| phone-missed | chat transcript call record + chat list preview (missed call) | drawn | - |
| phone-outgoing | chat transcript call record + chat list preview (call you made) | drawn | - |
| play-fill | 01-chat, 05-WideChats, WideChats | drawn | `play.svg` |
| plus | 01-chat, 04-contacts, 04-contacts-add, 05-WideChats (+10 more); §8.5 / 8.1 | drawn | `plus.svg` |
| qr-code | 07-Me | drawn | `qr-scan.svg` |
| search | 02-chatlist, 04-contacts, 04-contacts-add, 05-WideChats (+3 more); §8.8 | drawn | `search.svg` |
| shield | 07-Me | drawn | - |
| sparkle | 02-chatlist, 03-home, 05-Wide, 05-WideChats (+2 more); §8.14 | drawn (outline of sparkle-fill) | `sparkle.svg` |
| sparkle-fill | 02-chatlist, 03-home, 05-Wide, 05-WideChats (+4 more); §8.14 | drawn | `sparkle.svg` |
| speaker | 06-CallGroupVideo, 06-CallGroupVoice, 06-CallVideo, 06-CallVoice; §8.12 | drawn | `speaker.svg` |
| star | 03-home-edit | drawn | - |
| star-fill | contact page star (starred), phone Contacts row, Home Favorites widget | drawn (star, filled) | - |
| sun | 07-Me | drawn | - |
| tab-chats | 02-chatlist, 03-home, 03-home-edit, 04-contact-detail (+10 more); §8.2 | drawn | `message.svg` |
| tab-chats-fill | 02-chatlist, 05-WideChats, WideChats; §8.2 | drawn (selected tab) | - |
| tab-contacts-fill | 04-contacts, 04-contacts-add, 05-WideContacts, WideContacts; §8.2 | drawn (selected tab) | - |
| tab-home | 02-chatlist, 04-contacts, 04-contacts-add, 05-WideChats (+4 more); §8.2 | drawn | - |
| tab-home-fill | 03-home, 05-Wide, Wide; §8.2 | drawn (selected tab) | - |
| tab-me | 02-chatlist, 03-home, 04-contacts, 04-contacts-add (+7 more); §8.2 | drawn | `me.svg` |
| user | 03-home-edit | drawn | - |
| user-plus | 04-contacts, 04-contacts-add, 05-WideContacts, WideContacts | drawn | - |
| video | 01-chat, 04-contact-detail, 05-WideChats, 05-WideContacts (+11 more); §8.12 | drawn | `video.svg` |
| archive | phone context menus (chat row Archive / Unarchive) | drawn | `archive.svg` |
| block | phone context menus (chat row Block / Unblock) | drawn | - |
| check-circle | phone context menus (chat row Mark as read) | drawn | - |
| check | phone checkboxes, the white tick inside a checked circle | drawn | - |
| circle-dot | phone context menus (chat row Mark as unread) | drawn | - |
| eraser | phone context menus (chat row Clear history) | drawn | - |
| log-out | phone context menus (chat row Leave group) | drawn | - |
| more | phone Chats list swipe action (More) | drawn | - |
| pin | phone context menus (chat row Pin / Unpin) | drawn | `pin.svg` |
| reply | phone context menus (message Reply) | drawn | - |
| retry | chat picture bubble while it uploads: the disc after a failed send (DEV/PLAN-picture-upload-bubble.md) | drawn | - |
| share | phone context menus (shared item Share) | drawn | `share.svg` |
| trash | phone context menus (Delete) | drawn | `trash.svg` |
| arrow-up-right | phone sheet headers (ask-AI "Open in AI chat") | drawn | - |
| photo | 09-sheets-unify; phone small sheet (attach Photo) | drawn | - |
| file | 09-sheets-unify; phone small sheet (attach File) | drawn | - |
| send | 09-sheets-unify; contact page "Group admin" card (Re-invite) | drawn | `send.svg` |
| user-minus | 09-sheets-unify; contact page "Group admin" card (Remove from group) | drawn | - |
| unknown-caller | incoming-call page before the caller is known: filled soft silhouette on the blue avatar circle (120x120, translucent white, clipped to the circle) | drawn | - |
| clock | phone chat window, beside our message while it is being sent (DEV/PLAN-message-send-status.md) | drawn | - |
| alert-circle | phone chat window, beside our message that did not go out (tap: why) (DEV/PLAN-message-send-status.md) | drawn | - |
