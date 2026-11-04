# Sourced by the include checks. capabilities/<name>/src mirrors src/ (projects/capability-repos CR022), so a
# path a check names under src/ may sit in any capability folder as well.
#
# src_trees PATH: PATH under $ROOT, plus capabilities/*/src/<rest> when PATH is src or src/<rest>; only the
# paths that exist, absolute, one per line.
src_trees() {
  local path="$1"
  local rest=""
  case "$path" in
    src) rest="" ;;
    src/*) rest="${path#src}" ;;
    *)
      [[ -e "$ROOT/$path" ]] && echo "$ROOT/$path"
      return 0
      ;;
  esac
  local tree
  for tree in "$ROOT/src" "$ROOT"/capabilities/*/src; do
    [[ -e "$tree$rest" ]] && echo "$tree$rest"
  done
  return 0
}
