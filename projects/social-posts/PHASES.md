# Social posts — phases

Ordering only; status lives in [CURRENT_STATE.md](CURRENT_STATE.md).

| Phase | Scope | Proves |
|-------|-------|--------|
| **P0** | This design: log format, trust ladder (S001–S016) | — |
| **P1** | Post log library: encode / sign / verify entries and head statements, lanes, validity rules, local store (`post_entries`); unit tests, no UI | the format |
| **P2** | First slice: publish a text + image post to the default relay and the author's devices; follow an account; a minimal feed that fetches, verifies and shows *claimed* time | the core loop |
| **P3** | Replies, reposts, likes (reactor's log), edits, deletes; counts "as seen by" a relay or your follows | social actions |
| **P4** | Witness receipts from publish targets; *witnessed* time | weak time |
| **P5** | pp-ledger anchors (needs a pp-ledger anchor transaction design); *anchored* time, inconsistency flags | trusted time |

Distribution (publish targets in the account card, relay post API, head discovery) is designed in
detail before P2; social actions before P3.
