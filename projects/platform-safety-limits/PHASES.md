# Phased roadmap

Can proceed **in parallel** with [chat-storage v2a](../chat-storage-and-memory/PHASES.md). No hard dependency unless sharing `HttpClient` changes.

---

## Phase p1 — Shared limits header + HttpClient

**Goal:** One header; generic HTTP body cap.

- [ ] Add `PlatformLimits.h` with constants from DESIGN.md
- [ ] `HttpClient`: optional `max_response_bytes`; default `kMaxHttpClientBodyBytes`
- [ ] Unit test: reject or abort read when over cap

**Exit criteria:** HttpClient GET/POST cannot grow unbounded `std::string` without explicit opt-out.

---

## Phase p2 — LLM client

**Goal:** Bound `LlmClient` request and response.

- [ ] Cap response in `WriteCallback` at `kMaxLlmResponseBytes`
- [ ] Reject request `body.dump()` > `kMaxLlmRequestBytes` before curl
- [ ] Clear error message to UI via existing agent error path

**Exit criteria:** Malicious or runaway model output cannot OOM the app.

---

## Phase p3 — Profile JSON stores

**Goal:** Safe load for `identity.json` and `contacts.json`.

- [ ] Check file size ≤ `kMaxProfileJsonFileBytes` before parse
- [ ] Optional: max contacts count (e.g. 10_000) — open O004

**Exit criteria:** Corrupt/huge profile files fail with error, not OOM.

---

## Phase p4 — Parser and MCP

**Goal:** Cap local AI output and MCP payloads.

- [ ] `StructuredTextParser`: fail if RML output > `kMaxStructuredParserOutputBytes`
- [ ] `CallMcpToolJson`: cap result string at `kMaxMcpToolResultBytes`
- [ ] `user_payload` check in `MessageRouter` at `kMaxUserPayloadBytes`

**Exit criteria:** Single turn cannot write multi-megabyte `content_rml` to store.

---

## Changelog

| Date | Change |
|------|--------|
| 2026-06-29 | Project created; split from chat-storage audit |
