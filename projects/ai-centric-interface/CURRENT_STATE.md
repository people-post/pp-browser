# Current state — AI-centric interface

Inventory of what exists today relative to [DESIGN.md](DESIGN.md). Update when landing phase work.

**As of:** 2026-08-09

## Release posture

| Layer | Today | Target (v1) |
|-------|-------|-------------|
| Intent model | Implicit via `ResponseGoal` + ad-hoc planner rules | Explicit act × domain × commitment × horizon |
| Planner tool awareness | Live catalog injected into planner prompt (`SummaryForPrompt` + domain/risk tags) | Keep catalog; add act field on `TurnPlan` |
| Tool registration | `IToolProvider` + providers: messaging / web_search / MCP / **settings (Govern)** | Domain metadata + policy on every provider |
| Tool permissions | In-chat park + `tool_permissions` prefs (I005); Me → Security reset | Optional per-tool Settings editor; planner `commitment` field |
| Mutations from NL | Gated Ask for write tools; PeerId Operate path still weak | Operate tools + confirm payloads |
| Every act covered | No — Monitor/Decide/Repair/Govern thin or absent in planner | Thin path for all 10 |

## Turn pipeline (exists)

Documented in [docs/ui/AGENT_CONVERSATION.md](../../docs/ui/AGENT_CONVERSATION.md):

```
user → context → Plan → Execute tools → Synthesize blocks → validate → UI
```

| Piece | Location | Notes |
|-------|----------|-------|
| `TurnPlan` | `src/lib/ai/TurnPlan.*` | `response_goal`, `tools`, `render_mode`, `synthesis_hints` — **no act/domain/commitment** |
| `TurnPlanner` | `src/feature/ai/TurnPlanner.cpp` | LLM JSON plan; live `tools_summary` in prompt |
| `IToolProvider` / `ToolRegistry` | `src/lib/ai/IToolProvider.h`, `ToolRegistry.*` | MCP-shaped in-process registration (layer-safe for settings/messaging) |
| `PayloadTurnPlanBuilder` | `src/lib/ai/PayloadTurnPlanBuilder.*` | Fast path for article/form/tool chip payloads |
| `TurnExecutor` | `src/feature/ai/TurnExecutor.cpp` | Runs planned tools; permission gate; `people_list` shortcut |
| `ToolPermissionPolicy` | `src/feature/ai/ToolPermissionPolicy.*` | allow / ask / deny from prefs + session grants |
| `ParkedApproval` | `src/feature/ai/ParkedApproval.h` | Single in-flight in-chat confirm |
| `AgentSession` | `src/feature/ai/AgentSession.*` | Plan → execute → (park\|synthesize); resume permission |
| `PromptBuilder` | `src/lib/ai/PromptBuilder.cpp` | Planner + synthesis + chat-agent prompts |

## ResponseGoal (render — exists)

| Goal | Role today |
|------|------------|
| `display_feed` | Feed → long_list / working set |
| `summarize` | Article summary shape |
| `answer_question` | Answer-first paragraphs |
| `headlines` | Headline list |
| `people_discovery` | People long_list + Message/Add chips |
| `general` | Catch-all |

This is **not** an intent taxonomy. Operate/Navigate/Monitor/etc. are missing as first-class labels.

## Tools (exists)

Registered via `IToolProvider` → `ToolRegistry::RegisterProvider`:
`WebSearchProvider`, `McpToolProvider` (`BuildToolRegistryFromConfig`), `MessagingToolProvider`, `SettingsToolProvider`:

| Tool | Closest act(s) |
|------|----------------|
| `web_search` | Inquire, Discover (Knowledge) |
| `search_people` | Discover (People) |
| `list_contacts` | Discover / Monitor one-shot (People) |
| `add_contact` | Operate (People) — **directory_hit only** |
| `list_conversations` | Discover / Navigate (Communications) |
| `open_conversation` | Navigate (Communications) |
| `start_conversation` | Operate / Navigate (Communications) |
| `register_user` | Govern / Operate (Identity) |
| `update_profile_nickname` | Govern (Identity) |
| MCP / `blog_articles` | Discover / Inquire (Feeds) |
| `get_preferences` / `get_profile_identity` / `get_*` (settings) | Govern / Inquire (read) |
| `set_appearance` / `set_language` / `set_notifications` / `set_group_invite_policy` / … | Govern (Ask) |
| `probe_reachability` / `set_node_enabled` / `set_mesh_capabilities` | Govern / Monitor (Ask) |
| `reset_tool_permissions` | Govern / Repair (Ask) |

Non-goals for settings tools: PIN change, profile wipe, MCP/LLM secret writes, service endpoint hijacks.

UI payloads (not always planner tools): `add_contact`, `start_conversation`, `secure_message`, `show_contact`, `open_conversation`, form/article chips — see `ContactActionDispatcher`, `PeopleDiscoveryBlocks`.

## Known failure: PeerId add-contact

| Step | What happens |
|------|----------------|
| User | “Add contact of this peer id: 12D3KooW…” |
| Planner (typical) | `people_discovery` + `search_people` |
| Tool | Directory miss or irrelevant hits |
| Missing | Operate tool args for raw `peer_id` |
| Store | `ContactsStore::AddFromDirectoryHit` only convenience path |

Root cause: **Discover-shaped planning + Operate API that only accepts directory hits.**

## Gaps vs DESIGN taxonomy

| Act | Gap |
|-----|-----|
| Inquire | Mostly OK |
| Discover | OK for people/feeds; planner lacks full tool catalog |
| Produce | OK (blocks) |
| Operate | PeerId / NL mutations weak; planner rules omit mutations |
| Navigate | Tools exist; NL rarely plans them |
| Monitor | No watcher; no honest stub in planner rules |
| Decide | Blocks exist (`choice`/`poll`); no planner act |
| Govern | Tools exist; not framed as Govern |
| Repair | No dedicated path |
| Relate | OK as empty-tools general |

| Agency | Gap |
|--------|-----|
| Suggest / Confirm / Execute | In-chat tool permission park (I005) + people chips; not yet a `commitment` plan field |
| Autonomous | Not present |
| Horizon Session/Background | Not present |

## Tests / fixtures

| Asset | Location |
|-------|----------|
| Turn plan parse fixtures | `src/lib/ai/tests/fixtures/turn_plans/` (`people_discovery`, `headlines`, `chitchat_no_tools`) |
| Payload plan tests | `src/lib/ai/tests/payload_turn_plan_test.cpp` |
| Prompt builder tests | `src/lib/ai/tests/prompt_builder_test.cpp` |

No per-act corpus yet.

## Next agent — start here

1. Finish d0: record I001+ in [DECISIONS.md](DECISIONS.md); confirm v1 matrix in DESIGN.
2. Implement v1 per [PHASES.md](PHASES.md) — start with planner catalog + act field + PeerId Operate path.

## Working set panel — follow-ups (from docs/ui/WORKING_SET_PANEL.md)

| Feature | Approach |
|---------|----------|
| **Transient drill-down** | Row tap → [`ShellHost::PushTransient`](../../src/gui/shell/ShellHost.h) with detail RML; list stays in auxiliary |
| **Pin working set** | User pins panel content; new replies update chat but not panel until unpinned |
| **Multi-block navigator** | Tabs or list when one message has 2+ eligible blocks |
| **Explicit `surface` field** | Block JSON or TurnPlan overrides heuristics |
| **Multi-select in panel** | Selection state on working set model; batch actions back to chat/agent |
| **Persist working set anchor** | Store `entry_id + block_index` in thread metadata for reload |
| **Rename pane** | `preview` → `working_set` in ViewCatalog / RegisterPane |

---

### Phase 1 changed (history)

| File | Change |
|------|--------|
| `src/common/ui/WorkingSetTypes.h` | Core types |
| `src/base/ai/WorkingSetPolicy.h/.cpp` (new) | Goal + block routing |
| `src/base/ai/StructuredTextParser.h/.cpp` | Candidates, dual render, teasers |
| `src/feature/ai/AgentSession.h/.cpp` | Pass `response_goal` / `render_mode` in `AgentEvent` |
| `src/gui/chat/WorkingSetController.h/.cpp` | Sticky lifecycle, apply/open/clear |
| `src/gui/chat/ChatController.h/.cpp` | Owns WorkingSetController; RML callbacks + reply hooks |
| `assets/views/preview.rml` | Dynamic working set chrome |
| `assets/themes/base.rcss` | Panel + chip styles |
| `src/base/ai/PromptBuilder.cpp` | Side panel guidance for LLM |
| `docs/ui/WINDOW_SHELL.md`, `docs/ui/CHAT_TEMPLATES.md` | Cross-links and behavior notes |
| `tests/structured_text_parser_test.cpp`, new policy test | Coverage |
