# Diagnostics: what may leave the device

**Tier:** contract  
**Decision:** [privacy P009](../../projects/privacy/DECISIONS.md#p009--diagnostics-leave-the-device-as-shareable-lines-only)  
**Code:** `common/ShareableLine.h` (the rule), `common/Diag.h` / `common/Metrics.h` (the channels), `foundation/diagnostics/DiagnosticsBundle.*` (what is read), `app/DiagnosticsUpload.*` (the send)

A user can send diagnostics to support from Me → Security → Send diagnostics. This page is the whole list of what that sends, and the rule that keeps names, content and raw ids out of it.

## What is sent

| Source | File | Written by |
|--------|------|------------|
| Troubleshooting events | `{data_dir}/logs/diag.log` (this run) and `diag.1.log` (the previous run) | `DiagLine` — channel `Diag` |
| Operational metrics | `{data_dir}/logs/metrics.log` (this run) | `MetricsLine` — channel `Metrics` |
| The last crash, if one is pending | `{data_dir}/diagnostics/crash_pending.txt`, rebuilt as `Crash` lines: reason, frames, breadcrumb tags (never the raw sample) | `CrashEnvelopeLines` |

Nothing else. The developer log (`pp-browser.log`) is never read for a send: it is free text, and it stays on the device.

Before sending, the bundle keeps only lines of the channels `Diag`, `Metrics` and `Crash` at INFO or above, caps them (30 000 lines, 2 MiB JSON, 2048 bytes a line; at most 4 file entries — the gateway and app-support refuse a fifth; the least wanted file's oldest lines go first — `diag.1.log`, then `metrics.log`, then `diag.log`), and runs `SelfCheck` over exactly what would go: one hit (a long hex run, an id-like token, an email, a home path, one of the user's own or contacts' ids) and nothing is sent.

## The rule: shareable lines

Every line in those files is a `ShareableLine`: `event=<name> k=v k=v…`. Its builder takes only:

| Value | Method | Written as |
|-------|--------|------------|
| A word of the source code — a literal | `Add(key, "word")` (`ShareWord`, checked at compile time: letters, digits, `_ . - : < >`) | the word |
| A number, bool, duration | `Add(key, n)` | `n`, `1`/`0`, whole ms |
| A runtime string from a fixed list | `Pick(key, value, kWords)` with `constexpr ShareWord kWords[] = {"a", "b"}` (a braced list at the call is refused: MSVC cannot build it, C7595) | the listed word, `other`, or `-` when empty |
| An id (account, device, peer, relay user, thread) | `Ref(key, id)` | 8 hex of a keyed hash; the key is random per process and kept in memory only, so refs join lines of one run and nothing else |
| A random id (a call's) | `RandomId(key, id)` | its first 8 hex (every device that has it logs the same, to join them) |
| An error | `Fail(key, error)` | `category.code` — never the message |

There is no method that takes free text, so a name, a message, a search query, a path or an error message cannot be written into these files: the code does not compile. Event names and keys are literals too. Streaming into the channel loggers directly (`DiagLog().info << …`, `getLogger("Diag")`) is refused by `scripts/check/check_shareable_channels.sh` in CI.

### Adding an event

1. Pick the channel: `MetricsLine` for timings, rates and device load; `DiagLine` for what support needs to explain a failure.
2. Name it `area.what` (`relay.http_failed`), and add it to the table below in the same change.
3. A detail that needs text (an error message, a peer's address) goes to the developer log beside it, not into the line.
4. A new `ShareableLine` method is a change to this contract: review it as one.

## Diag events

| Event | Keys | Where |
|-------|------|-------|
| `network.baseline` | `online`, `transport` (NetworkTransport int), `expensive` | `MeshNetworkReaction` |
| `network.change` | `gen`, `was_online`, `online`, `was_transport`, `transport`, `expensive`, `attachment_changed` | `MeshNetworkReaction` |
| `mesh.udp_socket` | `reason` (`network` / `foreground`), `was_alive`, `reopened`, `alive` — the mesh's UDP socket check (#416; a dead one is reopened on its port) | `MeshNetworkReaction` (`ReportUdpSocketCheck`) |
| `relay.http_failed` | `action` (`send` / `poll`), `status` | `HttpRelayClient` |
| `relay.poll_failed` | `error` | `MeshDeliveryOrchestrator` |
| `relay.poll_too_large` | `n` | `MeshDeliveryOrchestrator` |
| `net.ui_block` | `route` (the request URL's first two path segments with `/` as `.`, one of a fixed list — `api.relay`, `api.llm`, `api.support`, `mcp`, `v1.users`, `v1.devices`, `v1.register`, … (`kUiBlockRoutes`); any other path is `other`; never the host, query or a later segment), `ms` (the whole request) — a blocking HTTP request ran on the UI thread and held every frame until it ended (the app sets the probe; node tools and tests do not). Also a `net.ui_block` metric with `ms` | `HttpClient` (every entry point) |
| `chat.send_failed` | `failure` (`SendFailureWord`), `thread` (ref) | `MeshDeliveryOrchestrator` |
| `chat.receive_failed` | `reason` (`ReceiveFailureWord`), `sender` (ref) | `MeshDeliveryOrchestrator` |
| `control.unverified` | `reason` (`no_card` — none kept for the sender, none riding that speaks for it / `device_not_on_card` — the card checked with does not list the sending device / `signature_mismatch` — the device's key from the card does not verify the record: a forgery, or a sender on a build before sign v4 / `invalid` — no sending device, a malformed signature or key), `card` (`kept` / `carried` / `none`: which card it was checked with), `card_refused` (a card rode but did not speak for the sender), `copy` (`account` / `device`), `held` (1: kept in memory for the sender's card and checked again when one is kept — only `no_card` / `device_not_on_card`), `sender` (ref) — a control-lane record's sender did not check ([multi-device M025](../../projects/multi-device-account/DECISIONS.md#m025--lane-cards-told-per-device-a-lane-record-waits-for-its-senders-card), #555). The developer log beside it has the card's `issued_at`, the sending device's prefix and the route | `RelayReceivePipeline::ReportUnverifiedControl` |
| `control.unreadable` | `copy` (`account` — the account pair's session / `device` — our pair with the sending device), `epoch` (the copy's), `held_epoch` (the session we hold, 0: none), `held` / `offered` (the first 8 hex of the session id, `AutoKeySessionId`, we hold / the copy's `key_init` opens; `-`: none — the same value on both devices of the pair, so a sender's and a receiver's lines join), `sender` (ref), `error` — a verified control-lane copy (call signaling, card and copy asks) did not open ([M024](../../projects/multi-device-account/DECISIONS.md#m024--the-control-lane-goes-per-device-a-calls-media-key-rides-the-copy)). The lane moves on (the relay cursor is not held: the copy will not open later); an account copy also shows the sender our card | `RelayReceivePipeline::ReportUnreadableControl` |
| `ai.llm_key` | `reason` (`401` — the gateway refused the Brief LLM Bearer), `outcome` (`minted` — a guest key was minted and stored beside the registered key; the turn asks once more with it / `reused` — the stored key had already changed (another turn renewed it), the turn asks once more with that one, nothing minted / `mint_failed` — no new key (mint or save failed), `error` set; the turn ends in its error / `retry_401` — the question asked again was refused too; the registered key is kept) — at most one mint until a turn is answered again; the refused registered key is dropped only after the retry was answered (B183) | `AgentSession` (brief stream turn) |
| `unlock.start` | — (a PIN or silent unlock began) | `ProfileUnlockGate` |
| `unlock.done` | `unlocked`, `ms` (integer ms since the door's first `unlock.start` — a wrong PIN and the retry do not restart it, so it includes the time spent retyping; a grace-window unlock: since that grace attempt began; a grace attempt that did not unlock is not counted in the PIN unlock after it; 0 only when neither was seen) | `ProfileUnlockGate` |
| `unlock.failed` | `wrong_pin`, `silent` (no PIN was typed) | `ProfileUnlockGate` |
| `unlock.error` | `error` | `ProfileUnlockGate` |
| `unlock.grace` | `result` (`hit` / `expired` / `missing` / `unavailable` — the PIN grace window read at a cold start or retried; `error` — the read failed, `error` set; `write_failed` — keeping or extending the window after a PIN unlock / in the foreground failed, `error` set, the unlock is not affected) ([at-rest A013](../../projects/at-rest-crypto/DECISIONS.md#a013--pin-grace-window-a-pin-typed-a-short-while-ago-is-not-asked-again)) | `ProfileUnlockGate`, `Application` (`write_failed` of a foreground extension) |
| `unlock.biometric` | `result` (`hit` — Face ID / Touch ID opened the vault; `canceled` — the prompt was canceled; `failed` — the face / finger did not pass or biometry is locked out; `missing` — the entry or `biometric.bin` is gone or does not open (enrolment changed, DEK replaced): both pieces deleted, the switch reads off; `unavailable` — the prompt could not run now (app sent to the background, a call); `error` — the read failed, `error` set) ([at-rest A014](../../projects/at-rest-crypto/DECISIONS.md#a014--face-id--touch-id-unlock-the-dek-behind-the-enrolled-biometry)) | `ProfileUnlockGate` |
| `call.system` | `what` (`push` / `push_locked` / `push_in_app` (iPhone: a VoIP push with the app open and unlocked — its CallKit call ended at once, the app rings itself) / `answer_pending` / `answer_ring` / `decline_pending` / `timer_armed` / `timer_fired` / `messaging_ready` / `ring_found` / `choice_other_call` (a ring other than the call the push named: the choice made on the push's call is kept for its own ring) / `accept_applied` / `decline_applied` / `answer_settling` (answered on the phone's call screen, the app still taking it) / `pending_gone` / `pending_orphan` / `audio_on` / `audio_off`), `ms` (timer events: the wait) — the phone's call screen (iOS CallKit; on Android the bridge's events only) and its push call | `CallSystemUi_Ios.mm`, `CallSystemUiBridge` |
| `call.chip` | `what`: `attach` (`root_chip`: the gesture's element is the chip) / `press` (`settling`, `docked`, `laid_out`, `left`, `top`) / `drop` (the chip's box as let go `left`, `top`, `w`, `h`; the drag's translate `drag_dx`, `drag_dy` (`left` / `top` include it); `layer_w`, `layer_h`, `viewport_h`; `side`, `y` (permille), `target_left`, `target_top`; `root_chip`, `root_is_rebuilt`: the dropped element is the one the markup rebuilds) / `freeze` (`left`, `top`) / `settle` / `dock` (`top`, `laid_top`, `anchor` `left` / `right`, `settling`) / `reflow` (`top_before`, `top_after`, `docked`, `animated`) / `remount` (`held`, `reason` `mode` / `place` / `other`, `minimized`, `side`, `y`, `area_h`, `chip_h`, `rendered_top` (dp, or `corner` / `none`), `chip_on_screen`, `settling`) — the minimised call chip dragged and placed (#166); all lengths in whole dp | `ShellCallChromeGesture`, `ShellHost::RemountCallChromeNow` |
| `call.net` | `call`, `role` (`caller` / `callee`), `known`, `online`, `cellular`, `expensive` (the device's attachment as the calls last heard it), `v4_private`, `v4_public`, `v6_global`, `v6_other` (counts of this device's advertised addresses), `relays`, `relays_up`, `relays_down` (rendezvous relays the app offers / with a link up / the reach record holds down), `peer_state`, `peer_fail`, `peer_fails` (the reach record for the peer) | `CallMediaConnectCoordinator::Start` |
| `call.signal` | `call`, `phase` (`invite_in` / `accept_out` / `accept_in`), `age_ms` (since the call row was created on this device), `implicit` (accept inferred from the media hello), `v4_private`, `v4_public`, `v6_global`, `v6_other` (counts of the addresses the message carries) | `CallSessionWorkflow` |
| `call.reach` | `call`, `role`, `attempt`, `ok`; ok: `kind` (`direct` / `punched` / `relayed`), `family` (4 / 6 / 0: the dial book's preferred endpoint, best effort), `reused`; failed: `err` (`ReachErrWord`), `why` (`ReachWhyWord`: a fixed list classified from the failure chain); `ms` since the connect started | `CallMediaConnectCoordinator::OnLinkReady` |
| `call.relay` | `call`, `dir` (`out` / `in`), `relay` (ref: the relay the circuit chose / the peer announced) | `CallReachSignals` |
| `call.punch` | `call`, `phase` (`offer_out` / `offer_in` / `answer_out` / `answer_in` / `burst_start` / `burst_ok` / `burst_fail`), `window_ms`, `addrs` (count), `ms` | `CallReachSignals` (the signalling punch) |
| `media.device_stuck` | `kind` (`mic` / `speaker` / `camera`), `op` (`open` / `close` / `reopen`), `ms` — a device operation past its budget; that device family refuses requests until it returns ([L016](../../projects/media-client-layers/DECISIONS.md#l016--no-control-thread-waits-on-hardware-per-session-media-a-background-cleanup-devices-with-budgets)) | `MediaDeviceArbiter` (once per stuck operation) |
| `media.device_back` | `kind`, `op`, `ms` (how long it took) — a stuck device operation returned; the family takes requests again | `MediaDeviceArbiter` |
| `media.codec_stuck` | `role` (`encoder` / `decoder`), `op` (`create` / `configure_encoder` / `configure_decoder` / `encode` / `decode` / `set_bitrate` / `reset`), `ms` (the budget) — a hardware codec call past its budget; the codec is dropped and replaced | `GuardedVideoCodec` |
| `call.result` | `call`, `role`, `ok`, `attempts`, `ms`, `kind`, `family`; on a give-up also `err` (`MediaErrWord`), `reach_err`, `why`, `relays`, `relays_up`, `peer_state`, `peer_fail`, `peer_fails` | `CallMediaConnectCoordinator::Finish` |

Call events are capped at 8 lines per event and call (`CallDiagBudget`), and a call's `call` value is the same on both devices, so a caller's and a callee's lines join on it. `relay` and every other ref join only within one device's run. Not visible from here (inside pp-cpp-mesh / pp-cpp-amp): each direct dial and circuit attempt (which relay, in what order, its own outcome), the Amp punch's start and outcome, how many relays hold our reservation.

Metrics events are listed where they are emitted (`MetricsLine("…")`); they follow the same rule.

## The request

`POST {BriefOrigin()}/api/support/diagnostics` (`DiagnosticsUploadUrl()`; dev-only override `PP_BROWSER_DIAGNOSTICS_URL`), `Content-Type: application/json`, `X-PP-Diagnostics: 1`, `Authorization: Bearer <brief LLM key>` (registered, else guest).

```json
{
  "schema_version": 1,
  "client_token": "<uuid, kept across a retry that got no answer>",
  "product": "pp-browser",
  "version": "1.2.3",
  "os": "macos",
  "captured_at": 1761642000000,
  "files": [
    { "name": "diag.log", "lines": ["[2026-10-28 08:57:32.329] [INFO] [Diag] event=relay.http_failed action=send status=503"] },
    { "name": "metrics.log", "lines": ["…"] },
    { "name": "diag.1.log", "lines": ["…"] },
    { "name": "crash", "lines": ["[…] [INFO] [Crash] reason=SIGSEGV"] }
  ]
}
```

| Answer | Shown as |
|--------|----------|
| 2xx `{"upload_id", "lines"}` | Sent; the reference is kept in `diagnostics/last_upload.json` (owner-only) and shown under the button |
| 429 `{"reason": "quota_key" \| "quota_ip" \| "quota_global"}` | Already sent today / limit reached |
| 409 | The previous send is still being processed |
| 401, 404, 503 | Unavailable |
| 413 | Too large |
| other (400: a body the server refused) / no answer | Failed / no network (the same `client_token` on retry) |

### The receiving side

The gateway (www, `app/src/r_support_client.ts`) checks the Bearer like the LLM routes, validates the body (the fields above, strict; 1–4 `files`; 30 000 lines, 2048 chars a line, 2.5 MB of line bytes) and forwards it to app-support `POST /v1/diagnostics` with two hashes for the quota: `X-Diag-Key-Hash` (SHA-256 of the Bearer) and `X-Diag-Ip-Hash` (HMAC with a gateway-only secret; IPv6 by /64). Its access log for this path carries no IP, user agent or request id. app-support validates the same body, reserves the daily quotas (3 a key, 10 an IP, a global cap), writes each line to CloudWatch as a `client_diag` record (`upload`, `file`, `version`, `os`, `line`) and keeps only the upload record in Mongo. A change to the body or the file limits is a change on all three.

Changelog: 2026-10-30 — first version (P009). 2026-10-31 — call connection events (`call.*`). 2026-11-07 — `MediaErrWord` gains `dropped` (a connected call's media ended under it, K014). 2026-11-07 — `ai.llm_key` (B183). 2026-11-08 — `control.unreadable` (M024, #555 B186); `control.unverified` (M025, #555). 2026-11-08 — unlock.done gains ms (time since unlock.start). 2026-11-08 — `net.ui_block` (HTTP on the UI thread; the messaging-ready requests moved off it). 2026-11-08 — `unlock.done` `ms` counts from the door's first `unlock.start` (not the last attempt) and times grace unlocks. 2026-11-08 — `call.chip` (#166, the minimised call chip's placement).
