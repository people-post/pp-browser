# Group chat — current state

**Last updated:** 2026-10-30 (G015 mentions m1–m2 on PR #413; signed roster + member check G012 / G013 merged in #411)

Ordering and checkboxes: [PHASES.md](PHASES.md). Read [DECISIONS.md](DECISIONS.md) G012–G015 before touching rosters or mentions.

## Landed

| Area | State |
|------|-------|
| Base group chat (C0–C5) | Create / invite / accept / remove / leave / transfer / rename / fork; N ciphertexts per message; roster in `GroupRosterStore` |
| Per-pair numbering (G010 / G011) | Implemented |
| Owner-signed roster (G012) | `lib/messaging/GroupRosterStatement.*`; roster events carry the statement, `owner_transferred` the signed hand-over; re-signed after `member_left` (#411) |
| Member check (G013) | `lib/messaging/GroupRosterSync.*`; request / reply on the control lane (`GroupRosterSyncFamily`); sends refused while checking / removed (#411) |
| Groups in the account backup | Codec and restore path in place and lab-tested, **but restore is unsupported in the product** (at-rest A011): no backup store is configured, so the member check has no product entry point today |
| Mentions m1 — wire (G015) | Content type 6 `text_spans` encode / decode (`TextSpans.*`, `ChatPayloadCodec`), tests `text_spans_test.cpp` — PR #413 |
| Mentions m2 — alerts (G015) | `MessageMentionsUs` (`MentionAlert.*`); `MeshDeliveryOrchestrator::InboundMentionsUs` tags background notices; `ChatController` lets a mention through a mute once a minute per group — PR #413 |

## Open (next first)

1. **m5 sending** — nobody can send a mention yet: facade API, composer `@` picker (public nickname), transcript highlight. Without it m2–m4 are untestable end to end.
2. **m2 held mentions** — today a mention in a checking group is simply not alerted; G015 says hold and decide when the check settles (< 10 min).
3. **m3 mute mentions too**, **m4 mention marker** (`thread_mentions`).
4. **m6** probe command + lab scenario; promote and mark G015 implemented.
5. **G014** size limit (proposed, independent).

## Notes for whoever picks this up

- DATA_LAYOUT already lists `thread_mentions` and `thread_local_prefs.mute_mentions` (written with the design, ahead of the code). Match them when m3 / m4 land, or fix the doc in the same PR.
- The mention-alert rule reads the roster on arrival; the transcript highlight must re-check at display (a late roster restores highlights).
- `@everyone` eligibility is the owner in `group_metadata.owner_identity`, which follows the signed roster (G012) — do not trust a role field from the message.
- Lab group scenarios: `./scripts/test/pp_local_test.sh run --suite app --no-build` with `PP_APP_LAB_ARGS="--only <scenario>"`.
