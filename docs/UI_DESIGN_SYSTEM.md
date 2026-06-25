# UI Design System

Source of truth for pp-browser theming, spacing, and component styling. AI-generated UI must reuse classes documented here rather than inventing raw hex colors.

## Principles

- **Reliable** — loading, pending, failed delivery, and offline states use the same semantic colors everywhere (`semantic-warning`, `semantic-error`, delivery indicators).
- **Responsive** — shell layout switches at 768dp (C++ `ShellLayout`); touch targets stay at least 44dp on compact layout.
- **Hierarchy through space** — importance is expressed with size, weight, spacing, and elevation—not decoration.

## Aesthetic

Productivity-oriented (Notion/Slack): neutral surfaces, one primary blue accent, restrained saturation, clear pane separation.

## Surface inventory

| Surface | RML / module | Themed elements |
|---------|--------------|-----------------|
| Shell chrome | `window_shell.rml`, `ShellHost` | Banner, toast, activity strip, scrims, toolbar |
| Navigation | `sidebar.rml` | Session rows, unread badges, footer |
| Primary chat | `chat.rml` | Header, bubbles, empty state, E2E chrome |
| Composer | `composer.rml` | Prompt card, send button |
| Working set | `preview.rml` | Panel, chips, long-list rows |
| Settings | `settings.rml` | Sections, fields |
| Feedback | `ShellFeedback` | Dialog, confirm, banner |

## Spacing scale (4dp grid)

| Token | Value | Typical use |
|-------|-------|-------------|
| `space-1` | 4dp | Tight inline gaps |
| `space-2` | 8dp | Row gaps, small margins |
| `space-3` | 12dp | Section padding, chat gutters |
| `space-4` | 16dp | Card padding, pane insets |
| `space-6` | 24dp | Panel padding, empty states |
| `space-8` | 32dp | Large section breaks |
| `space-12` | 48dp | Hero spacing |

Utility class: `.gap-sm` = 8dp bottom margin (legacy).

## Typography scale

| Class / token | Size | Use |
|---------------|------|-----|
| `.text-xs` | 12dp | Meta, timestamps, secure badge |
| `.text-sm` / `.btn`, `.field` | 14dp | Toolbar, labels, inputs |
| `body` / `.text-base` | 16dp | Body copy |
| `.text-lg` / `.settings-section-title` | 18dp | Section titles |
| `.text-xl` / `.chat-header-title` | 22dp | Pane titles |
| `.text-2xl` / `.heading-1` | 28dp | Hero headings |
| `.heading-2` | 20dp | Secondary headings |
| `.heading-3` | 16dp | Tertiary headings |

Font family: `LatoLatin` everywhere.

## Touch targets

Minimum **44dp** height/width on compact layout for:

- `.shell-toolbar-btn`
- `.sidebar-session` (increased from 36dp)
- `.sidebar-new-chat`, `.prompt-send-btn` (44dp)
- Primary action buttons in composer and forms

## Semantic color tokens

Hex values live in `assets/themes/colors-light.rcss` and `assets/themes/colors-dark.rcss` inside `@media (theme: light|dark)` blocks. RmlUi has no CSS variables; never hardcode hex in RML or AI-generated RCSS.

### Light mode

| Role | Hex | Used for |
|------|-----|----------|
| `surface-app` | `#f8f9fb` | App / pane background |
| `surface-elevated` | `#ffffff` | Cards, composer, dialogs, inputs |
| `surface-muted` | `#f3f4f6` | Assistant bubble, secondary panels |
| `surface-overlay` | `#00000066` | Scrims |
| `text-primary` | `#1a1a1a` | Body text |
| `text-secondary` | `#333333` | Bubble copy, labels |
| `text-muted` | `#6b7280` | Hints, subtitles |
| `text-heading` | `#111111` | Headings |
| `text-inverse` | `#ffffff` | On primary / user bubble |
| `border-subtle` | `#e5e7eb` | Pane dividers, cards |
| `border-strong` | `#d1d5db` | Inputs, form borders |
| `accent-primary` | `#4a6cf7` | Primary buttons, user bubble, activity strip |
| `accent-primary-hover` | `#5a7cff` | Hover states |
| `accent-primary-active` | `#3f5ee0` | Pressed / calendar nav |
| `accent-secure` | `#0d9488` | E2E thread accent (teal) |
| `accent-secure-surface` | `#ecfdf5` | E2E header tint (light) |
| `accent-secure-border` | `#99f6e4` | E2E composer border |
| `semantic-success` | `#15803d` | Success status |
| `semantic-warning-bg` | `#fef3c7` | Banner background |
| `semantic-warning-border` | `#fcd34d` | Banner border |
| `semantic-warning-text` | `#92400e` | Banner copy |
| `semantic-error` | `#dc2626` | Errors |
| `semantic-info-bg` | `#eef2ff` | Info callout |
| `semantic-info-highlight` | `#f5f7ff` | Highlighted card |
| `selection-bg` | `#b4d5fe` | Text selection (default) |
| `selection-user` | `#ffffff55` | Selection in user bubble |
| `toast-bg` | `#1f2937` | Toast background |

### Dark mode

| Role | Hex | Used for |
|------|-----|----------|
| `surface-app` | `#111827` | App / pane background |
| `surface-elevated` | `#1f2937` | Cards, composer, dialogs |
| `surface-muted` | `#374151` | Assistant bubble |
| `surface-overlay` | `#00000099` | Scrims |
| `text-primary` | `#f9fafb` | Body text |
| `text-secondary` | `#e5e7eb` | Bubble copy |
| `text-muted` | `#9ca3af` | Hints |
| `text-heading` | `#ffffff` | Headings |
| `text-inverse` | `#ffffff` | On accents |
| `border-subtle` | `#374151` | Dividers |
| `border-strong` | `#4b5563` | Inputs |
| `accent-primary` | `#6366f1` | Primary actions |
| `accent-primary-hover` | `#818cf8` | Hover |
| `accent-primary-active` | `#4f46e5` | Active |
| `accent-secure` | `#2dd4bf` | E2E accent |
| `accent-secure-surface` | `#134e4a` | E2E header tint |
| `accent-secure-border` | `#0f766e` | E2E composer border |
| `semantic-success` | `#4ade80` | Success |
| `semantic-warning-bg` | `#422006` | Banner |
| `semantic-warning-border` | `#854d0e` | Banner border |
| `semantic-warning-text` | `#fde68a` | Banner text |
| `semantic-error` | `#f87171` | Errors |
| `semantic-info-bg` | `#1e1b4b` | Info callout |
| `semantic-info-highlight` | `#312e81` | Highlight card |
| `selection-bg` | `#3b5998` | Selection |
| `selection-user` | `#ffffff33` | User bubble selection |
| `toast-bg` | `#374151` | Toast |

## E2E vs public threads

Moderate visual distinction at the chat shell level (not a global theme swap).

| Element | Public (`.chat-shell--public`) | E2E (`.chat-shell--e2e`) |
|---------|-------------------------------|--------------------------|
| Header | Standard pane surface | Subtle secure surface tint + 4dp left border (`accent-secure`) |
| Title row | Thread title only | Title + lock label (`.chat-secure-badge`) |
| Subtitle | Kind description | "End-to-end encrypted" |
| Composer | `border-subtle` | Stronger secure border |

Data binding: `thread_encrypted` on chat model; `data-class-chat-shell--e2e="thread_encrypted"` on chat root.

## Component classes (reuse before adding rules)

### Layout

`.stack`, `.row`, `.gap-sm`, `.card`

### Typography

`.text`, `.heading-1`, `.heading-2`, `.heading-3`, `.muted`, `.error`, `.text-xs`

### Controls

`.btn`, `.btn-primary`, `.btn-secondary`, `.btn-ghost`, `.field`

### Chat

`.chat-panel`, `.chat-header`, `.chat-shell--e2e`, `.chat-shell--public`, `.bubble-user`, `.bubble-assistant`, `.bubble-peer`, `.prompt-composer`, `.chat-suggestion`, `.chat-form`, `.chat-callout`, `.chat-callout-warning`, `.chat-working-set-chip`

### Shell

`.shell-pane`, `.shell-toolbar`, `.shell-banner`, `.shell-toast`, `.shell-dialog`

### Sidebar

`.sidebar-panel`, `.sidebar-session`, `.sidebar-session-active`, `.sidebar-unread`

## RCSS file layout

```
assets/themes/
  base.rcss           # Documents entry; RML links split files directly
  foundation.rcss     # Spacing, typography structure (no theme colors)
  components.rcss     # Component layout (no theme colors)
  colors-light.rcss   # @media (theme: light) { ... }
  colors-dark.rcss    # @media (theme: dark) { ... }
```

Runtime: `Theme::ApplyAppearance` activates `light` or `dark` on the RmlUi context. Default user preference: **System** (follows `SDL_GetSystemTheme`).

## AI generation rules

- Use classes from this document; do not emit `#hex` in generated RCSS.
- Do not use `@media` in AI-generated stylesheets.
- Prefer `.card`, `.row`, `.stack`, `.btn-primary` over bespoke rules.

See also [RCSS_PROFILE.md](RCSS_PROFILE.md) and [RML_PROFILE.md](RML_PROFILE.md).
