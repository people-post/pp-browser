#!/usr/bin/env bash
# Re-import the pinned ISC quirc decoder; preserves local CMake and provenance.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
git clone --quiet --depth 1 --branch v1.2 https://github.com/dlbeer/quirc.git "$work/quirc"
test "$(git -C "$work/quirc" rev-parse HEAD)" = 542848dd6b9b0eaa9587bbf25b9bc67bd8a71fca
mkdir -p "$root/third_party/quirc/lib"
cp "$work/quirc"/lib/*.[ch] "$root/third_party/quirc/lib/"
cp "$work/quirc/LICENSE" "$root/third_party/quirc/LICENSE"
