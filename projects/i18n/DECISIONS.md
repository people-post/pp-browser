# Multi-language UI — decisions

## I001 — JSON catalogs + `Tr(key)`, not gettext

**Date:** 2026-07-15  
**Decision:** Ship locale JSON under `assets/locales/{tag}.json` and resolve strings through `LocalizationService::Tr`. Do not introduce gettext/`.po` or ICU MessageFormat in v1.  
**Rationale:** nlohmann/json is already vendored; assets hot-path matches themes/fonts; translators and agents can edit JSON without extra tooling. Simple `{name}` interpolation covers v1 needs. ICU can be revisited if plural/gender rules become product requirements.

## I002 — Language is a profile preference next to appearance

**Date:** 2026-07-15  
**Decision:** Persist `language` on `ProfilePreferences` (`preferences.json`), default `"system"`. Place the control under Me → Appearance (not a separate top-level settings section in v1).  
**Rationale:** Same lifetime as theme/appearance; per-profile UI prefs already flow through `SessionStore`. Appearance is the natural “look & feel” home. A dedicated Language section can split out later if the section grows.

## I003 — Native self-names in the language picker

**Date:** 2026-07-15  
**Decision:** Picker rows show each language in its native name (English / 简体中文) regardless of the current UI language. The “System” option is translated via `Tr()`.  
**Rationale:** Users scan for their language even when the app is currently in the wrong language. Translating “简体中文” into English as “Simplified Chinese” only is a worse recovery path when stuck in an unfamiliar locale.

## I004 — Bottom sheet picker via ContextMenuHost, not `<select>` alone

**Date:** 2026-07-15  
**Decision:** Language selection uses `ContextMenuHost::ShowActions` (bottom action sheet on compact/mobile, floating list on expanded). Extend actions with optional selected/checkmark. Do not use RmlUi `<select>` as the only mobile affordance.  
**Rationale:** Product requirement for bottom-up modal on mobile; `ContextMenuHost` already implements that presentation. Theme can keep `<select>` (short fixed list); language picker should match mobile platform convention and scale to more locales.

## I005 — UI language ≠ assistant language (v1)

**Date:** 2026-07-15  
**Decision:** v1 localizes chrome and settings only. LLM prompts (`PromptBuilder`) and model replies stay as-authored; we do not auto-force assistant output to the UI language.  
**Rationale:** Mixing i18n with model prompting is a separate product decision (user may chat in English while wanting a Chinese shell, or reverse). Track assistant language as a follow-up if needed.

## I006 — BCP-47 tags; `zh-Hans` not `zh` / `zh-CN`

**Date:** 2026-07-15  
**Decision:** Catalog and preference tags are BCP-47. Simplified Chinese is `zh-Hans`. Prefer script subtags over region when the product difference is script.  
**Rationale:** Clearer for later `zh-Hant`; avoids implying PRC-specific content when the strings are generic Simplified Chinese UI copy.

## I007 — English fallback is mandatory

**Date:** 2026-07-15  
**Decision:** Missing keys in a non-English catalog fall back to English. Missing English keys return the key string (debug log) rather than crashing.  
**Rationale:** Partial translations must remain usable during incremental localization (phases i3–i5).

## I008 — CJK font is a hard prerequisite for shipping zh-Hans

**Date:** 2026-07-15  
**Decision:** Do not expose Simplified Chinese in the picker until a licensed CJK fallback font is loaded. Phase i4 can land in the same release train as i2/i3, but shipping ZH without glyphs is a blocker.  
**Rationale:** Latin-only Lato will show tofu; that is worse than omitting the locale.
