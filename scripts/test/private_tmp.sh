#!/bin/sh
# Runs a command with a private TMPDIR so parallel ctest processes never share
# std::filesystem::temp_directory_path(). Not exec'd, so the directory is removed on exit.
set -eu

base="${TMPDIR:-/tmp}"
TMPDIR="$(mktemp -d "${base%/}/pp-test.XXXXXX")"
export TMPDIR

child=""
cleanup() { rm -rf "$TMPDIR"; }
trap cleanup EXIT
trap '[ -z "$child" ] || kill -TERM "$child" 2>/dev/null; exit 143' TERM
trap '[ -z "$child" ] || kill -INT "$child" 2>/dev/null; exit 130' INT

"$@" &
child=$!
# Exit code of the child is propagated; a trapped signal interrupts `wait`.
rc=0
wait "$child" || rc=$?
exit "$rc"
