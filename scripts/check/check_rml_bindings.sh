#!/usr/bin/env bash
# data-rml sets inner RML: the bound value is parsed as markup and a `{{…}}` in it is evaluated
# (B78). Plain text is bound as `{{expr}}`; data-rml is only for markup fields named `*_rml`.
# Checked in the shipped views and in the markup src/ serializes (ShellHost, ContextMenuHost, …).
# See docs/ui/RML_PROFILE.md.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
FAIL=0

# Either quote style; the field may be escaped inside a C++ string literal (data-rml=\"x\").
# A field name is required: the guard test's own error message contains an empty `data-rml=""`.
PATTERN='data-rml=\\?["'"'"']([^"'"'"'\\]+)\\?["'"'"']'

check_tree() {
  local label="$1"
  local glob="$2"
  local path="$3"
  local hits
  hits="$(rg --no-heading --line-number --only-matching --glob "$glob" "$PATTERN" "$ROOT/$path" 2>/dev/null \
    | grep -Ev 'data-rml=\\?["'"'"'][^"'"'"'\\]*_rml\\?["'"'"']$' || true)"
  if [[ -n "$hits" ]]; then
    echo "FAIL: $label — bind plain text as {{expr}}; data-rml is only for *_rml fields"
    echo "$hits" | sed "s|^$ROOT/||"
    FAIL=1
  fi
}

check_tree "data-rml on a text field in assets/views" '*.rml' assets/views
check_tree "data-rml on a text field in assets/samples" '*.rml' assets/samples
check_tree "data-rml on a text field in markup built in src/" '*.{h,hpp,cpp,cc,mm}' src

if [[ "$FAIL" -ne 0 ]]; then
  exit 1
fi
echo "OK: data-rml is only used for *_rml fields"
