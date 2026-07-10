# Design — AI-centric interface

## Vision

The product is an **AI-centric interface**: conversation is the primary way users interact with the system and, over time, with **anything** reachable through tools (local stores, P2P, MCP, OS, web).

The UI is not a dashboard of features first — it is a shell that:

1. Classifies what the user wants (**act**).
2. Routes to the right capability slice (**domain**).
3. Chooses how far to go without asking (**commitment**).
4. Chooses whether work ends this turn or continues (**horizon**).
5. Renders a reply appropriate to that plan (**response_goal** / blocks / working set).

This project owns the **intent + agency + planner contract**. Individual domains (messaging, feeds, crypto UX) stay in their feature projects; they register tools and domain labels here.

## Principles

1. **Acts are closed; domains are open.** New apps/MCP servers add domains and tools, not new top-level acts.
2. **v1 ships a path for every act.** Prefer a dumb path (confirm chip, clarify, one-shot status) over a missing act.
3. **Render ≠ intent.** Existing `ResponseGoal` steers reply shape / working set; it must not absorb Operate vs Discover.
4. **Mutations are explicit.** Operate / Repair / Govern default to Confirm unless allowlisted.
5. **Never invent identifiers.** Tools and payloads carry real ids from prior lookup or user text that was validated.
6. **Improve in place.** Later phases deepen an act without renaming the taxonomy.

## Taxonomy

### Acts (10 — closed)

| Act | User wants to… | Includes | Must not confuse with |
|-----|----------------|----------|------------------------|
| **Inquire** | Understand | Q&A, explain, teach-me | Discover (finding items) |
| **Discover** | Find what exists | Search, browse, list, inspect candidates | Operate (changing state) |
| **Produce** | Make or remake an artifact | Draft, summarize, translate, generate UI/code/media | Operate (sending / committing) |
| **Operate** | Change the world | Add contact, send, delete, call API, register | Navigate (only switching focus) |
| **Navigate** | Move attention | Open thread/file/url/app | Operate (create vs open) |
| **Monitor** | Stay aware over time | Status now, watch, alerts | Discover (one-shot search) |
| **Decide** | Pick among options | Compare, recommend, plan next steps | Operate (doing the pick) |
| **Govern** | Steer the system itself | Settings, permissions, identity, memory, tool policy | Operate on external world |
| **Repair** | Restore correctness | Debug, undo, verify, audit, recover | Operate (forward progress) |
| **Relate** | Social / phatic / meta | Thanks, vent, “what can you do?” | Inquire (real information need) |

### Domains (open)

Domains are **labels + tool families**, not a frozen enum. Kind buckets for registration:

| Kind | Examples (today → later) |
|------|---------------------------|
| People & social | contacts, directory, presence |
| Communications | threads, chat, (later email/calls) |
| Time & commitments | (later calendar, reminders, tasks) |
| Knowledge | web search, docs, notes, RAG |
| Feeds | blog/MCP article tools |
| Media | (later images/audio/video) |
| Code & projects | (later repos/PRs) |
| Files & data | (later FS/DB) |
| Devices & environment | (later OS/apps/IoT) |
| Identity & security | registration, nicknames, PSK, fingerprints |
| Agent-self | tools list, memory, modes, permissions |
| General | no clear slice |

pp-browser v1 domains in scope: **People**, **Communications**, **Knowledge/Web**, **Feeds**, **Identity & security**, **Agent-self**, **General**.

### Commitment (4)

| Level | Meaning |
|-------|---------|
| **Suggest** | Propose only (text / chips); no side effect |
| **Confirm** | Args ready; user approves via chip/payload |
| **Execute** | Run now (args complete, policy allows) |
| **Autonomous** | Continue in background under a charter |

Defaults (v1 policy):

| Act | Default commitment |
|-----|--------------------|
| Relate, Inquire, Decide, Produce | Suggest (Produce may Execute for local-only artifacts) |
| Discover, Navigate | Execute (lookup / open) |
| Operate, Repair, Govern | Confirm (allowlist may Execute) |
| Monitor | Confirm setup → Autonomous later; v1 often one-shot Execute status |

### Horizon (3)

| Horizon | Meaning |
|---------|---------|
| **Turn** | Finished in this reply |
| **Session** | Multi-step in this chat |
| **Background** | Outlives the turn (watchers, agents) |

v1: almost everything is **Turn**. Session/Background are specified so Monitor/Decide-plan are not forgotten; Autonomous Monitor is post-v1.

## Planning contract

Extend the turn plan (conceptually; field names TBD in implementation):

```
act            — one of the 10
domain         — string label (open)
commitment     — suggest | confirm | execute | autonomous
horizon        — turn | session | background
tools[]        — as today
response_goal  — render (existing)
render_mode    — blocks | people_list | …
synthesis_hints
user_request
```

### Decision procedure

1. Classify **act** (default **Relate** if purely social; **Inquire** if a real question with no better fit).
2. Classify **domain**.
3. Choose **commitment** / **horizon** from act defaults + risk.
4. If Discover / Inquire → plan **lookup** tools as needed.
5. If Operate / Navigate / Govern / Repair → if args complete → Confirm or Execute mutation/nav tools; else Ask (Clarify) or Discover first.
6. If Monitor → v1 one-shot status tool or honest “watching not available yet”.
7. If Decide → Produce options (choice/poll/action_list); do not Operate unless user confirms.
8. If Produce → usually no world tools; emit artifacts as blocks.
9. Set `response_goal` / `render_mode` from act×domain (render only).

### Motivating example

| Utterance | Classification | v1 behavior |
|-----------|----------------|-------------|
| Add contact of this peer id: `12D3KooW…` | **Operate × People × Execute/Confirm** | Call peer-id-capable `add_contact` (or confirm chip with structured payload) — **not** `search_people` → empty list |

## v1 simple coverage (every act exists)

Rule: each act has **at least one** path. Prefer reuse of current tools/blocks. Mark maturity `[thin]` vs `[real]`.

| Act | v1 simple solution | Maturity |
|-----|--------------------|----------|
| **Inquire** | Planner → optional `web_search` / MCP → `answer_question` blocks | `[real]` (exists) |
| **Discover** | `search_people` / `list_contacts` / feed tools → `people_list` or `long_list` | `[real]` (exists) |
| **Produce** | Synthesis emits blocks (paragraph, code, form, …); no commit tools | `[real]` (exists) |
| **Operate** | Mutation tools +/or confirm chips (`add_contact`, `start_conversation`, `register_user`, …); **extend add-contact for PeerId** | `[thin]` → must fix PeerId path in v1 |
| **Navigate** | `open_conversation` / `list_conversations` + open payloads | `[thin]` (tools exist; planner rarely uses them for NL) |
| **Monitor** | One-shot “status” via existing list tools **or** honest Clarify: watching not supported yet | `[thin]` stub OK |
| **Decide** | `choice` / `poll` / `action_list` blocks; no auto-Operate | `[thin]` (blocks exist; planner hints) |
| **Govern** | `register_user`, `update_profile_nickname`; else point to Settings / Me tab | `[thin]` |
| **Repair** | Explain error + Suggest undo/retry chips when a known repair payload exists; else Inquire-style diagnosis | `[thin]` stub OK |
| **Relate** | Empty tools; `general` blocks; capability blurb from tool summary | `[real]` (exists) |

### v1 tool / prompt work (minimum)

1. **Wire `tools_summary` into the planner prompt** (today discarded).
2. **Planner rules for all 10 acts** (lookup vs mutate vs navigate vs stub).
3. **Operate×People:** accept PeerId (and optionally display name) on `add_contact` / ContactsStore path — not only `directory_hit`.
4. **Confirm path:** prefer structured chip payloads for risky Operate when commitment=Confirm.
5. **TurnPlan fields** for `act` / `domain` / `commitment` / `horizon` (or encode in `synthesis_hints` only as a temporary bridge — prefer real fields).
6. **Fixture corpus:** one NL example per act for planner tests.

## Post-v1 improvement tracks

Improve **one row at a time** without changing act names:

| Track | Examples |
|-------|----------|
| Operate depth | Idempotent add, multiaddr-required dial, batch actions |
| Monitor real | Subscriptions, background AgentSession jobs, notification chips |
| Decide real | Multi-criteria compare tables, plan → Confirm Operate pipeline |
| Govern real | In-chat settings forms bound to config; memory forget; tool allowlists |
| Repair real | Undo last mutation; verify fingerprint; gap/integrity repair hooks |
| Domain expansion | Calendar, files, MCP apps as new domain labels |
| Autonomous | Horizon=Background charters with kill switch |
| Fast paths | Regex/payload builders for PeerId, URLs, deep links (skip planner) |

## Non-goals (this project)

- Replacing RmlUi / window shell design systems
- Owning E2E crypto or SQLite thread store (sibling projects)
- Inventing unbounded new `ResponseGoal` values for every act
- Full OS automation in v1

## Success criteria

### d0 (design)

- [x] Taxonomy documented (10 acts, open domains, commitment, horizon)
- [ ] Accepted in [DECISIONS.md](DECISIONS.md) (I001+)
- [ ] v1 coverage matrix reviewed (no act without a path)

### v1

- [ ] Planner prompt includes live tool catalog + act rules
- [ ] Turn plan carries act (and ideally domain/commitment/horizon)
- [ ] NL “add contact by PeerId” succeeds (Operate path)
- [ ] Fixture: ≥1 planner example per act
- [ ] Monitor/Repair thin paths never silently no-op without user-visible explanation

### Later

- [ ] Stable summary folded into `docs/` (e.g. `docs/AI_CENTRIC_INTERFACE.md`) when v1 ships
