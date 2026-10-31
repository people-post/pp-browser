# Social posts — phases

Ordering only; status lives in [CURRENT_STATE.md](CURRENT_STATE.md).

| Phase | Scope | Proves |
|-------|-------|--------|
| **P0** | This design: log format, trust ladder (S001–S016); distribution (S017–S021); reposts, moderation, post body, notices (S022–S029) | — |
| **P1** | Post log library: encode / sign / verify entries and head statements, lanes, validity rules, local store (`post_entries`); unit tests, no UI | the format |
| **P1b** | The post body of S026 (media alt / size / duration / placeholder, facets, content warning and labels, languages, `reply_root`) and the entry extension section (S027) in `lib/posts`; vectors regenerated, the relay's parser to match — before anything is published | the format, final for v1 |
| **P2** | First slice: publish a text + image post to the default relay and the author's devices; follow an account; a minimal feed that fetches, verifies and shows *claimed* time. Relay: served authors only, refusal codes, withheld stubs, the scan step before media is served, reports | the core loop |
| **P3** | Replies and threads (*more replies* on tap), reposts and quotes with carried copies, likes (reactor's log), edits, deletes; notices from follows; counts "as seen by" a relay or your follows | social actions |
| **P4** | Witness receipts from publish targets; *witnessed* time | weak time |
| **P5** | pp-ledger anchors (needs a pp-ledger anchor transaction design); *anchored* time, inconsistency flags | trusted time |

Distribution (publish targets in the head statement, relay post API, head discovery) is designed in
detail before P2; social actions before P3. After P3, unordered: reply control, `article`, account lists,
share links, labelers, bylines ([DESIGN § 4 Later](DESIGN.md#later-p3-and-after)).
