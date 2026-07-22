# Liquid Glass — phases

Execute in order. Each phase should land reviewable PRs; do not batch lg1+lg5 into one change.

---

## lg0 — Project docs + ADRs

- [x] `projects/liquid-glass/` README, DESIGN, CURRENT_STATE, DECISIONS, PHASES
- [x] Register in `projects/README.md`

---

## lg1 — Layout foundation (floating compact chrome)

**Goal:** Content scrolls **under** bottom nav; no blur yet — validate overlap geometry and safe area.

### Tasks

- [x] Change `.shell-bottom-chrome` to absolute bottom overlay with horizontal inset (see DESIGN.md)
- [x] Add `.shell-nav-page` / chat body `padding-bottom`: nav height (56dp) + safe-area bottom
- [x] Plumb safe-area bottom into shell layout (SDL display event or existing `ProfilePreferences.safe_area` if wired)
- [x] Ensure `#shell-composer-mount` on Home tab respects bottom inset (composer above nav pill visually)
- [x] Verify interruption / dismiss order unchanged (`ShellInterruption` tests)
- [x] Update `shell_host_test` if layout visibility flags change
- [ ] Manual QA: compact Home, Sessions overlay, Me list — no content permanently hidden behind nav

### Files (expected)

- `assets/themes/components.rcss`
- `src/feature/ui/ShellHost.cpp` (optional padding in serialized markup)
- `src/render/integration/host/BrowserHost.cpp` or shell sync for safe area (if not already exposed to RML)

### Exit criteria

- Resizing to &lt;768dp: last chat line scrolls above nav; nav floats with margin; no functional regressions.

---

## lg2 — Frosted glass materials (RCSS)

**Goal:** Real `backdrop-filter` blur + translucency on compact chrome surfaces.

### Tasks

- [x] Add glass tokens to `colors-light.rcss` / `colors-dark.rcss`
- [x] Add `.surface-glass` / `.surface-glass--opaque` utilities in `components.rcss`
- [x] Apply to `.shell-nav-rail--compact` (or inner wrapper)
- [x] Apply to `.shell-chat-overlay-chrome`, `.shell-transient-chrome`
- [x] Remove or soften opaque `background-color` / `border-top` rules that fight glass in compact selectors
- [ ] Side-by-side screenshot vs iOS 26 tab bar (Simulator) — tune blur radius and fill alpha **(deferred)**
- [ ] Profile scroll FPS desktop + one Android device; record baseline in CURRENT_STATE **(deferred → lg6)**

### Exit criteria

- Scrolling chat visibly blurs through bottom nav on compact Home.
- Dark and light themes acceptable; no parse errors in RmlUi log.

---

## lg3 — Liquid shape + edge treatment

**Goal:** Apple-like floating pill and specular edge without custom shaders. **LG003 = C.**

### Tasks

- [x] Nav pill: `border-radius` 22–28dp, horizontal inset 12–16dp, optional `box-shadow`
- [x] Active tab capsule inside pill (adjust `.shell-nav-tab--active` for compact)
- [x] Specular top border via `glass-border-highlight` token
- [x] Chat overlay header: full-bleed frosted bar (absolute over messages)
- [x] Auxiliary sheet: full-body glass + lighter scrim (LG003=C)
- [x] Composer glass strip on Home compact + chat overlay

### Exit criteria

- Visual review: reads as “floating glass control,” not flat toolbar with blur. **(deferred)**

---

## lg4 — Motion + scroll coupling

**Goal:** Subtle material response to scroll (Apple polish).

### Tasks

- [ ] Define motion spec: e.g. nav opacity 0.92→1.0 when scroll idle; blur 16→20px when scrolling (bounds TBD)
- [ ] Implement via RCSS `transition` where sufficient, or C++ dirty on scroll events from chat/sidebar controllers
- [ ] Respect reduce-motion pref → static material
- [ ] Ensure no jank on Android background/foreground (invalidate blur on GL reset)

### Exit criteria

- Motion visible but not distracting; disabled when reduce motion on.

---

## lg5 — Renderer extensions (optional, Apple-faithful)

**Goal:** Specular / liquid edge beyond stock blur — only if lg2–lg3 fail side-by-side iOS review.

### Tasks

- [ ] Spike: custom `filter: shader(liquid_glass, ...)` in `RmlUi_Renderer_GL3.cpp`
- [ ] Parameters: tint, highlight strength, corner radius uniform
- [ ] Apply only to `.surface-glass` elements (not global)
- [ ] GLES parity test (Android + iOS build)
- [ ] Document fork delta in `RMLUI_UPSTREAM.md`
- [ ] Fallback to lg2 blur if shader compile fails

### Exit criteria

- Stakeholder sign-off vs iOS reference; fallback path still works.

---

## lg6 — Fallbacks, perf gates, agent docs

### Tasks

- [ ] **Reduce transparency** → `.surface-glass--opaque` on all glass surfaces
- [ ] **Low GPU** heuristic (optional): disable backdrop-filter on known-weak devices or when FPS drops
- [ ] Feature flag or compile flag `PP_BROWSER_LIQUID_GLASS` default ON desktop, evaluate mobile default
- [ ] Complete test matrix from DESIGN.md
- [ ] Block merge if perf gate fails (LG005 device)

### Exit criteria

- All accessibility fallbacks manual-tested; perf documented in CURRENT_STATE.

---

## lg7 — Promote to stable docs

- [ ] Add **Materials / Liquid Glass** section to `docs/ui/UI_DESIGN_SYSTEM.md`
- [ ] Update `docs/ui/WINDOW_SHELL.md` compact chrome section (overlay layout + safe area)
- [ ] Update `docs/ui/RCSS_PROFILE.md` — glass utilities theme-only
- [ ] Mark project README status **Done**; freeze DECISIONS; trim CURRENT_STATE to “shipped” summary

---

## Agent batch delivery order

When resuming work in focused sessions:

1. **Session A:** lg1 only (layout) — done.
2. **Session B:** lg2 (frosted materials) — done (visual QA deferred).
3. **Session C:** lg3 (pill shape + edges, LG003=C) — done (visual QA deferred).
4. **Session D:** lg6 fallbacks + lg7 docs (can parallel lg4 motion if owner wants polish).
5. **Session E (optional):** lg5 renderer spike — time-boxed; abandon if &gt;3 days without visual win.

Do not start lg5 until lg2–lg3 screenshots are reviewed against iOS reference.
