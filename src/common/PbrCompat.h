#pragma once

/**
 * pp-browser compatibility: `src/common` owns namespace `pp`; the app remains
 * in `pbr`. Each common public header re-exports its API into `pbr` (see
 * trailing blocks) so existing call sites keep compiling until common moves
 * to pp-cpp-common and consumers switch to `pp::` directly.
 *
 * Prefer namespace aliases / using-declarations (qualified lookup) — do not use
 * `using namespace ::pp` inside `pbr` (that does not create `pbr::Name`).
 *
 * Product helpers that left common (EmojiKey, StartupTiming, WorkerDispatch)
 * live under `base/` in `pbr` and are not re-exported here.
 *
 * This header is documentation-only; it is safe to include but defines nothing.
 */
