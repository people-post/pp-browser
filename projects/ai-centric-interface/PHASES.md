# Phased roadmap — AI-centric interface

Check boxes when work is **merged and verified**. Design (d0) should be solid before large prompt/tool rewrites.

**Strategy:** v1 makes **every act exist** with a simple path; later phases deepen one act or domain at a time.

---

## Traceability

| Phase | Focus | Maturity |
|-------|-------|----------|
| d0 | Design baseline in this folder | Taxonomy + v1 matrix |
| v1a | Plan schema + planner prompt (acts + tool catalog) | Intent wiring |
| v1b | Operate×People PeerId path | Unblocks motivating bug |
| v1c | Thin paths for remaining acts + fixtures | Completeness |
| v1d | Docs promote + CURRENT_STATE refresh | Exit v1 |
| v2+ | Per-act upgrades (Monitor, Decide, Repair, …) | Depth |

---

## Phase d0 — Design baseline

**Goal:** Long-term model agreed; v1 coverage matrix has no empty acts.

- [x] Draft [DESIGN.md](DESIGN.md) taxonomy (10 acts, domains, commitment, horizon)
- [x] Draft [CURRENT_STATE.md](CURRENT_STATE.md) gap analysis
- [ ] Record I001–I003 in [DECISIONS.md](DECISIONS.md) (human/agent accept)
- [ ] Skim review: every act has a v1 row in DESIGN § v1 simple coverage

**Exit criteria:** README status can move to “d0 complete — v1 next”; no open taxonomy rename debates blocking v1a.

---

## Phase v1a — Planner contract

**Goal:** Planner sees tools and emits act (minimum viable intent).

- [x] Stop discarding `tools_summary` in `TurnPlanner::Plan`; inject into planner system prompt
- [ ] Extend `BuildPlannerPrompt` with the 10 acts + lookup vs mutate vs navigate vs stub rules
      (partial: lookup vs mutate guidance + live catalog tags landed with `IToolProvider`)
- [ ] Add `act` (required) to `TurnPlan` JSON schema; parse/validate; default `relate` or `inquire` if missing only during soft rollout if needed
- [ ] Optional v1a: `domain`, `commitment`, `horizon` fields (string enums) — prefer yes if cheap
- [ ] Keep `response_goal` as **render** only; map act×domain → suggested goal in planner rules
- [ ] Unit tests: parse plans with `act`; reject unknown act names

**Exit criteria:** Planner repair path understands new fields; logs/`TurnTrace` can show `act`.

**Anti-patterns:** Expanding `ResponseGoal` to 10+ values instead of adding `act`; putting tool calls inside blocks JSON.

---

## Phase v1b — Operate×People (PeerId)

**Goal:** NL add-by-PeerId works end-to-end.

- [ ] Extend `add_contact` tool (and/or ContactsStore helper) to accept `peer_id` (+ optional `display_name`) without requiring `directory_hit`
- [ ] Validate PeerId shape (e.g. libp2p base58 / `12D3KooW…` prefix heuristics + strict parse where available)
- [ ] Planner rule: clear “add contact” + PeerId → Operate + `add_contact`, not only `search_people`
- [ ] Optional: Confirm chip path with payload `{type:add_contact, peer_id:…}` in `ContactActionDispatcher`
- [ ] Test: tool execute + one planner fixture for the motivating utterance

**Exit criteria:** Motivating message adds a contact (or shows Confirm chip that adds it); no invented directory hits.

---

## Phase v1c — Thin coverage for all acts

**Goal:** Every act has a documented, testable path.

| Act | v1c checklist |
|-----|----------------|
| Inquire | [ ] Fixture + planner rule (existing web_search OK) |
| Discover | [ ] Fixture (people / feed) |
| Produce | [ ] Fixture: “draft …” → empty tools + produce-oriented hints |
| Operate | [ ] Covered by v1b + register/start_conversation rules |
| Navigate | [ ] Planner rule + fixture for “open conversation with …” / list then open |
| Monitor | [ ] Explicit thin path: one-shot list **or** Clarify “watching not available yet” — never silent fail |
| Decide | [ ] Hints to emit `choice`/`poll`/`action_list`; fixture |
| Govern | [ ] Rules for register/nickname; else Suggest Settings |
| Repair | [ ] Thin: explain + Suggest known repair chips if any; else honest limits |
| Relate | [ ] Fixture: chitchat → empty tools |

Also:

- [ ] Synthesis prompt: short act-specific reminder (optional if `synthesis_hints` enough)
- [ ] Update [CURRENT_STATE.md](CURRENT_STATE.md) gap table

**Exit criteria:** `src/base/ai/tests/fixtures/turn_plans/` (or equivalent) has ≥1 example per act; Monitor/Repair never no-op without user-visible text.

---

## Phase v1d — Stabilize

**Goal:** Hand off enduring facts to stable docs; close v1.

- [ ] Promote summary to `docs/AI_CENTRIC_INTERFACE.md` (or section under AGENT_CONVERSATION)
- [ ] Link from [AGENTS.md](../../AGENTS.md) / AGENT_CONVERSATION
- [ ] README status → **v1 done**; list v2+ tracks
- [ ] Note follow-ups in DECISIONS only if normative

**Exit criteria:** New agents read docs/ + this project CURRENT_STATE and know where intent lives.

---

## Phase v2+ — Improve one-by-one (unordered backlog)

Pick **one** track per effort; do not rename acts.

| Track | When to pull |
|-------|----------------|
| **Monitor real** | Need watchers / background jobs / notifications |
| **Decide → Operate pipeline** | Plans that end in Confirm chips for mutations |
| **Govern in-chat** | Settings forms / memory / tool allowlists via chat |
| **Repair real** | Undo last contact add; integrity/PSK repair hooks |
| **Operate depth** | Multiaddr dial requirements; batch; idempotency |
| **Fast paths** | PeerId/URL/deep-link `PayloadTurnPlanBuilder` without LLM |
| **Autonomous horizon** | Charters + kill switch (depends on product readiness) |
| **Domain packs** | Calendar, files, new MCP apps as registered domains |

Each track gets its own checklist subsection in this file when started.

---

## Agent session reading list

1. [DESIGN.md](DESIGN.md) — taxonomy + v1 matrix  
2. [CURRENT_STATE.md](CURRENT_STATE.md) — code map + gaps  
3. [docs/ui/AGENT_CONVERSATION.md](../../docs/ui/AGENT_CONVERSATION.md) — turn pipeline  
4. Relevant phase checklist above  
5. Update CURRENT_STATE + checkboxes in the same PR  

---

## Anti-patterns (cause rework)

- Treating `people_discovery` as the only people intent (blocks Operate)
- Requiring `directory_hit` for every contact mutation
- Planner without tool catalog (`(void)tools_summary`)
- Silent no-op for Monitor/Repair “for later”
- New top-level acts for each feature (“AddContactAct”) — use Operate × domain instead
