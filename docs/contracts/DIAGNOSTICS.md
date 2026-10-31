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
| A random call id | `Call(key, call_id)` | its first 8 hex (both sides log the same, to join them) |
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
| `relay.http_failed` | `action` (`send` / `poll`), `status` | `HttpRelayClient` |
| `relay.poll_failed` | `error` | `MeshDeliveryOrchestrator` |
| `relay.poll_too_large` | `n` | `MeshDeliveryOrchestrator` |
| `chat.send_failed` | `failure` (`SendFailureWord`), `thread` (ref) | `MeshDeliveryOrchestrator` |
| `chat.receive_failed` | `reason` (`ReceiveFailureWord`), `sender` (ref) | `MeshDeliveryOrchestrator` |
| `unlock.done` | `unlocked` | `ProfileUnlockGate` |
| `unlock.failed` | `wrong_pin`, `silent` (no PIN was typed) | `ProfileUnlockGate` |
| `unlock.error` | `error` | `ProfileUnlockGate` |

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

Changelog: 2026-10-30 — first version (P009).
