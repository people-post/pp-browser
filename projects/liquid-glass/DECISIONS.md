# Liquid Glass — decisions

Record dated outcomes. When shipped, mark **superseded by** stable doc — do not duplicate normative tables in two places.

---

## LG001 — Compact-only scope

**Date:** 2026-07-19  
**Status:** Accepted  
**Decision:** Liquid Glass applies only when `layout_mode == compact` (&lt;768dp). Expanded desktop shell keeps current flat productivity surfaces.  
**Rationale:** Apple Liquid Glass is a mobile idiom; expanded layout users expect dense pane separation (Notion/Slack). Reduces blast radius and GPU cost on desktop multi-pane.  
**Superseded by:** (pending) `docs/ui/UI_DESIGN_SYSTEM.md` Materials section

---

## LG002 — RmlUi backdrop-filter first; no native UIVisualEffectView in v1

**Date:** 2026-07-19  
**Status:** Accepted  
**Decision:** Implement glass via RCSS `backdrop-filter` and GL3/GLES renderer path shared with Android/desktop. Do not embed native iOS `UIVisualEffectView` or Android-only blur overlays in v1.  
**Rationale:** Single RML tree, one QA matrix, matches existing render stack. Native materials can be revisited if GLES path cannot reach fidelity on Apple Silicon iOS devices.  
**Superseded by:** —

---

## LG003 — Chrome surface inventory

**Date:** 2026-07-21  
**Status:** Superseded by [LG008](#lg008--floating-chrome-top-layer-frost-only) (sheet body opaque; frost on header strip only when top)  
**Decision:** **C (max)** — bottom nav pill + chat/transient headers + Home/overlay composer strip + **full auxiliary sheet glass body** + lighter scrim tint.  
**Rationale:** Highest material coverage for compact chrome; sheet body glass is acceptable GPU cost while the sheet is open. Visual QA / blur radius tuning deferred.  
**Superseded by:** (pending) `docs/ui/UI_DESIGN_SYSTEM.md` Materials section

**Options considered:**

| Option | Surfaces |
|--------|----------|
| A (minimal) | Bottom nav pill + chat/transient headers only |
| B | A + auxiliary sheet header strip + Home composer strip |
| **C (chosen)** | B + full sheet glass body + scrim tint adjustment |

---

## LG004 — Renderer fidelity path

**Date:** 2026-07-19  
**Status:** **Closed — skip lg5 (LG008, 2026-07-22)**  
**Decision:** Floating Chrome uses stock `backdrop-filter` on at most one surface. No custom shader spike.

---

## LG005 — Performance reference device (open)

**Date:** 2026-07-19  
**Status:** **Open — resolve before lg6 gate**  
**Decision:** Name one **minimum Android device** (or emulator profile) and one **iOS device/simulator** for FPS acceptance.  
**Placeholder:** Pixel 6a class / iPhone 15 Simulator — replace with owner hardware.  
**Gate:** Compact chat scroll avg frame time regression ≤ 4ms vs glass-off on reference Android.

---

## LG006 — Reduce transparency setting (open)

**Date:** 2026-07-19  
**Status:** **Open — resolve before lg6**  
**Options:**

| Option | Behavior |
|--------|----------|
| **A** | New Me → Accessibility toggle “Reduce transparency” |
| **B** | Follow system only (iOS `UIAccessibilityIsReduceTransparencyEnabled` — needs platform bridge) |
| **C** | A + system when bridge exists |

**Recommendation:** A for v1 (consistent cross-platform); add B when iOS shell matures.

---

## LG007 — Feature flag default (open)

**Date:** 2026-07-19  
**Status:** **Open — resolve at lg6**  
**Options:** Always on when compact | `ProfilePreferences` user toggle | compile-time `PP_BROWSER_LIQUID_GLASS`  
**Recommendation:** Profile pref default **on**, hidden from Settings until polish complete; allows internal dogfood off switch.

---

## LG008 — Floating Chrome: top-layer frost only

**Date:** 2026-07-22  
**Status:** Accepted  
**Decision:** Pivot from Apple-faithful Liquid Glass to **Floating Chrome**. All compact chrome uses opaque `.surface-chrome` by default. **At most one** visible chrome bar per frame may add `.surface-chrome--frost` (`backdrop-filter: blur(12px)`), selected by `ShellInterruption::CompactChromeFrostSurface` from the interruption stack. Cancel lg4 (scroll motion) and lg5 (custom shaders). Auxiliary sheet body is opaque; frost limited to top strip / headers.  
**Rationale:** Multi-surface blur caused O(N) backdrop passes during chat scroll; floating layout already delivers hierarchy. Top-layer-only frost preserves depth cue at bounded GPU cost.  
**Supersedes:** Apple-faithful targets in DESIGN.md; LG003=C full sheet glass; LG004 shader path; lg4/lg5 phases.
