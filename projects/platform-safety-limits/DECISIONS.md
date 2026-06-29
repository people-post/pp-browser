# Decisions log

---

## P001 — Separate non-chat limits from chat-storage D029

**Date:** 2026-06-29  
**Decision:** Chat wire, thread DB, relay ingest, and transcript UI limits live in **chat-storage-and-memory** (D029–D033). This project owns **LLM HTTP**, **profile JSON stores**, **MCP tool results**, **StructuredTextParser output**, and **generic HttpClient** defaults.  
**Rationale:** Clear ownership; relay envelope cap stays with messaging protocol.  
**Alternatives:** Single monolithic limits doc in chat-storage only.

---

## P002 — Platform limit constants (proposed)

**Date:** 2026-06-29  
**Decision:** Adopt limits in [DESIGN.md](DESIGN.md) § Target limits as v1 defaults. Implement in `PlatformLimits.h` when work starts.  
**Rationale:** Audit identified unbounded LLM responses and profile JSON as highest non-chat risk.  
**Alternatives:** Smaller 1 MiB LLM cap; no MCP cap.

---

## Open decisions

| ID | Question | Options |
|----|----------|---------|
| O001 | `PlatformLimits.h` location | `src/common/` vs `src/base/platform/` |
| O002 | Oversize LLM response behavior | Hard error vs truncate with user-visible notice |
| O003 | When to land vs chat v2a | Parallel track vs after SqliteThreadStore |

When resolved, move to numbered decisions above.
