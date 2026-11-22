#!/usr/bin/env bash
# This repository is public; the relay's deployment is not (pp-relay-apis and its infrastructure repositories are
# private). Public files describe contracts — formats, endpoints, vectors, what a client does — and say "the relay's
# deployment" where a detail is the deployment's: its secrets, cloud permissions and infrastructure repositories stay
# in the private repositories. AGENTS.md § Documentation authority.
#
# Fails on a tracked file that names one (PATTERN below), unless the file is in private_infra_allowlist.txt with
# exactly that many matching lines: the files from before this check, to move out (lower a count, or drop the line,
# as they are moved; never raise one).
#
#   check_private_infra.sh               check the tree
#   check_private_infra.sh --self-test
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
allowlist="${root}/scripts/check/private_infra_allowlist.txt"

# Secret ids and secret stores, cloud resource names and permissions, the private infrastructure repositories.
PATTERN='SECRET_ID|secretsmanager:|Secrets Manager|GetSecretValue|arn:aws|kms:|pp-relay-infra-(terraform|ansible)'

# The tree's tracked files; the self-test's files under a temporary directory (--no-index).
grep_index=()

# Matching lines per file, "path count", for the files under <dir>.
matches() { # matches <dir>
  (cd "$1" && git grep ${grep_index[@]+"${grep_index[@]}"} -I -c -E "${PATTERN}" -- . \
    ':!third_party' ':!projects/e2e-message-crypto/tools' \
    ':!scripts/check/check_private_infra.sh' ':!scripts/check/private_infra_allowlist.txt' 2>/dev/null |
    sed -E 's/^\.\///; s/:([0-9]+)$/ \1/' | sort) || true
}

check() { # check <dir> <allowlist>; prints problems, exits 1 on any
  local dir="$1" list="$2" path count allowed failed=0
  declare -A allow=()
  if [[ -f "${list}" ]]; then
    while read -r path count; do
      [[ -z "${path}" || "${path}" == \#* ]] && continue
      allow["${path}"]="${count}"
    done <"${list}"
  fi
  while read -r path count; do
    [[ -z "${path}" ]] && continue
    allowed="${allow[${path}]:-0}"
    unset 'allow[${path}]'
    if [[ "${allowed}" == 0 ]]; then
      echo "::error file=${path}::names the relay's private deployment (${count} lines: secret ids, cloud permissions or infrastructure repositories); say \"the relay's deployment\" and keep the detail in pp-relay-apis"
      (cd "${dir}" && git grep ${grep_index[@]+"${grep_index[@]}"} -I -n -E "${PATTERN}" -- "${path}" | sed 's/^/  /') || true
      failed=1
    elif ((count > allowed)); then
      echo "::error file=${path}::names the relay's private deployment on ${count} lines, more than its ${allowed} in private_infra_allowlist.txt: move the new ones out, not the count up"
      (cd "${dir}" && git grep ${grep_index[@]+"${grep_index[@]}"} -I -n -E "${PATTERN}" -- "${path}" | sed 's/^/  /') || true
      failed=1
    elif ((count < allowed)); then
      echo "::error file=${path}::now ${count} lines, allowed ${allowed}: lower its line in private_infra_allowlist.txt"
      failed=1
    fi
  done < <(matches "${dir}")
  for path in "${!allow[@]}"; do
    echo "::error::${path} no longer names the private deployment: drop its line from private_infra_allowlist.txt"
    failed=1
  done
  return "${failed}"
}

self_test() {
  local dir failures=0
  grep_index=(--no-index)
  dir="$(mktemp -d)"
  mkdir -p "${dir}/docs"
  expect() { # expect <0|1> <what>
    local got=0
    check "${dir}" "${dir}/allow.txt" >/dev/null || got=1
    if [[ "${got}" != "$1" ]]; then
      echo "self-test: $2: exited ${got}, wanted $1" >&2
      failures=$((failures + 1))
    fi
  }
  printf 'A relay certificate is served at GET /v1/relay-identity.\n' >"${dir}/docs/a.md"
  : >"${dir}/allow.txt"
  expect 0 "no match"
  printf 'The registry reads RELAY_CERTIFICATE_SECRET_ID.\n' >>"${dir}/docs/a.md"
  expect 1 "a new file naming a secret id"
  printf 'docs/a.md 1\n' >"${dir}/allow.txt"
  expect 0 "an allowed file at its count"
  printf 'IAM: secretsmanager:GetSecretValue, kms:Decrypt.\n' >>"${dir}/docs/a.md"
  expect 1 "an allowed file above its count"
  printf 'docs/a.md 3\n' >"${dir}/allow.txt"
  printf 'A relay certificate is served at GET /v1/relay-identity.\n' >"${dir}/docs/a.md"
  expect 1 "an allowed file below its count"
  printf 'docs/a.md 0\n' >"${dir}/allow.txt"
  printf 'The registry reads RELAY_CERTIFICATE_SECRET_ID.\n' >"${dir}/docs/a.md"
  expect 1 "a zero line is no allowance"
  printf '# a comment\ndocs/gone.md 2\n' >"${dir}/allow.txt"
  expect 1 "an allowed file that no longer matches"
  : >"${dir}/allow.txt"
  for line in 'arn:aws:secretsmanager:us-east-1:1:secret:x' 'the seed in Secrets Manager' 'see pp-relay-infra-terraform' \
    'pp-relay-infra-ansible roles'; do
    printf '%s\n' "${line}" >"${dir}/docs/a.md"
    expect 1 "pattern: ${line}"
  done
  printf 'pp-relay-apis serves the relay; Lambda and DynamoDB are its architecture.\n' >"${dir}/docs/a.md"
  expect 0 "the relay's public name and architecture"
  rm -rf "${dir}"
  [[ ${failures} -eq 0 ]] && echo "check_private_infra self-test: ok"
  [[ ${failures} -eq 0 ]]
}

case "${1:-}" in
  "") check "${root}" "${allowlist}" ;;
  --self-test) self_test ;;
  *)
    echo "usage: $0 [--self-test]" >&2
    exit 2
    ;;
esac
