#!/usr/bin/env bash
# The org relays' ids and certificates a build ships beside their origins (release-compatibility SIGNED_FORMATS § 3.3,
# § 3.5: a relay address is its origin and its relay id; the build's certificate is the floor a fresh install starts
# from). Reads PP_API_URL_STAGING / PP_API_URL_DEV and PP_RELAY_ID_PROD / _STAGING / _DEV from the environment, and
# the certificate files config/relay-certs/prod.cert / staging.cert / dev.cert (PP_RELAY_CERT_FILE_PROD / _STAGING /
# _DEV override the paths). The environments are dev, staging and prod (release-compatibility RC009, RC021).
#
# An unset or empty PP_RELAY_ID_<ENV> (a CI variable not set reads as empty) is what the build scripts pass as the
# build's default, "pending": that environment's operator set is not made yet, and the relay is not used (it fails
# closed). Only a CMake `-DPP_RELAY_ID_<ENV>=` given by hand builds a first-use (dogfood) relay.
#
#   check_relay_ids.sh release   a release (tagged installers, TestFlight, a published pp-node): fails when
#                                PP_RELAY_ID_PROD is not a relay id (empty, pending, malformed) or prod.cert holds no
#                                certificate; when PP_API_URL_STAGING (PP_API_URL_DEV) is set, the same for
#                                PP_RELAY_ID_STAGING and staging.cert (PP_RELAY_ID_DEV and dev.cert): the build
#                                ships that environment
#   check_relay_ids.sh dogfood   warns instead
#   check_relay_ids.sh --self-test
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

relay_id_form() {
  # `relay:` and base64url-unpadded of 32 bytes: 43 characters, the last one of the 16 whose spare bits are zero.
  [[ "$1" =~ ^relay:[A-Za-z0-9_-]{42}[AEIMQUYcgkosw048]$ ]]
}

# The certificate file's text without comment lines and blanks: base64, "pending", or empty.
certificate_text() {
  local file="$1"
  [[ -f "${file}" ]] || return 0
  grep -v '^[[:space:]]*#' "${file}" | tr -d '[:space:]'
}

check_env() { # check_env <ENV> <relay id> <certificate file>; prints problems
  local env="$1" id="$2" file="$3" cert
  if [[ "${id}" == "pending" ]]; then
    echo "PP_RELAY_ID_${env} is the placeholder (pending): the ${env} relay is not used until its operator set exists"
  elif ! relay_id_form "${id}"; then
    echo "PP_RELAY_ID_${env} is not a relay id (relay: and 43 base64url characters): ${id}"
  fi
  cert="$(certificate_text "${file}")"
  if [[ -z "${cert}" || "${cert}" == "pending" ]]; then
    echo "${file#"${root}/"} holds no certificate (a placeholder): a fresh install would know no audience before its first fetch"
  elif ! [[ "${cert}" =~ ^[A-Za-z0-9+/]+=*$ ]]; then
    echo "${file#"${root}/"} is not a certificate's base64"
  fi
}

check() {
  local mode="$1" problems=() line
  while IFS= read -r line; do
    [[ -n "${line}" ]] && problems+=("${line}")
  done < <(
    check_env PROD "${PP_RELAY_ID_PROD:-pending}" "${PP_RELAY_CERT_FILE_PROD:-${root}/config/relay-certs/prod.cert}"
    # Staging and dev only where the build compiles their origin (a store build ships prod only, RC015).
    if [[ -n "${PP_API_URL_STAGING:-}" ]]; then
      check_env STAGING "${PP_RELAY_ID_STAGING:-pending}" \
        "${PP_RELAY_CERT_FILE_STAGING:-${root}/config/relay-certs/staging.cert}"
    fi
    if [[ -n "${PP_API_URL_DEV:-}" ]]; then
      check_env DEV "${PP_RELAY_ID_DEV:-pending}" "${PP_RELAY_CERT_FILE_DEV:-${root}/config/relay-certs/dev.cert}"
    fi
  )
  local problem
  for problem in "${problems[@]+"${problems[@]}"}"; do
    if [[ "${mode}" == "release" ]]; then
      echo "::error::${problem}"
    else
      echo "::warning::${problem}"
    fi
  done
  [[ "${mode}" != "release" || ${#problems[@]} -eq 0 ]]
}

self_test() {
  local good="relay:AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8" failures=0 dir
  dir="$(mktemp -d)"
  printf '# a comment\nAQID\nBA==\n' >"${dir}/good.cert"
  printf '# placeholder\npending\n' >"${dir}/pending.cert"
  printf 'not base64!\n' >"${dir}/bad.cert"
  # expect <0|1> <mode> <prod id|UNSET> <prod cert> [<ENV> <url> <id|UNSET> <cert>]...: ENV is STAGING or DEV
  expect() {
    local want="$1" mode="$2" got=0 args="$*"
    (
      unset PP_RELAY_ID_PROD PP_RELAY_ID_STAGING PP_RELAY_ID_DEV PP_API_URL_STAGING PP_API_URL_DEV
      [[ "$3" == UNSET ]] || export PP_RELAY_ID_PROD="$3"
      export PP_RELAY_CERT_FILE_PROD="${dir}/$4"
      shift 4
      while [[ $# -ge 4 ]]; do
        export "PP_API_URL_$1=$2" "PP_RELAY_CERT_FILE_$1=${dir}/$4"
        [[ "$3" == UNSET ]] || export "PP_RELAY_ID_$1=$3"
        shift 4
      done
      check "${mode}"
    ) >/dev/null || got=1
    if [[ "${got}" != "${want}" ]]; then
      echo "self-test: check ${args} exited ${got}, wanted ${want}" >&2
      failures=$((failures + 1))
    fi
  }
  local staging="https://staging.example.test" dev="https://dev.example.test"
  expect 0 release "${good}" good.cert
  expect 1 release UNSET good.cert             # the placeholder fails closed
  expect 1 release pending good.cert
  expect 1 release "" good.cert                # empty: what CI passes, pending
  expect 1 release "relay:short" good.cert
  expect 1 release "QmTWkSQAcGsETogTFKrPJ3GzdHG3vP9UwXVyp2HeK9YBvP" good.cert # a PeerId is no relay id
  expect 1 release "relay:AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh9" good.cert # spare bits
  expect 1 release "${good}" pending.cert      # no certificate floor
  expect 1 release "${good}" bad.cert
  expect 1 release "${good}" missing.cert
  # Staging and dev count only where the build compiles their origin; then each needs its own id and certificate.
  expect 0 release "${good}" good.cert STAGING "" UNSET pending.cert DEV "" UNSET missing.cert
  for env in STAGING DEV; do
    local url="${staging}"
    [[ "${env}" == DEV ]] && url="${dev}"
    expect 0 release "${good}" good.cert "${env}" "${url}" "${good}" good.cert
    expect 1 release "${good}" good.cert "${env}" "${url}" UNSET good.cert
    expect 1 release "${good}" good.cert "${env}" "${url}" pending good.cert
    expect 1 release "${good}" good.cert "${env}" "${url}" "" good.cert
    expect 1 release "${good}" good.cert "${env}" "${url}" "relay:short" good.cert
    expect 1 release "${good}" good.cert "${env}" "${url}" "${good}" pending.cert
    expect 1 release "${good}" good.cert "${env}" "${url}" "${good}" bad.cert
    expect 1 release "${good}" good.cert "${env}" "${url}" "${good}" missing.cert
  done
  expect 0 release "${good}" good.cert STAGING "${staging}" "${good}" good.cert DEV "${dev}" "${good}" good.cert
  expect 1 release "${good}" good.cert STAGING "${staging}" "${good}" good.cert DEV "${dev}" UNSET good.cert
  expect 1 release "${good}" good.cert STAGING "${staging}" UNSET good.cert DEV "${dev}" "${good}" good.cert
  expect 1 release UNSET good.cert STAGING "${staging}" "${good}" good.cert DEV "${dev}" "${good}" good.cert
  expect 0 dogfood UNSET pending.cert STAGING "${staging}" "" pending.cert DEV "${dev}" UNSET missing.cert
  # The files in the tree: each environment's exists and holds a certificate or the placeholder (dev's is made, 1 of 1;
  # prod's and staging's are pending until Leo makes them).
  local name text
  for name in prod staging dev; do
    text="$(certificate_text "${root}/config/relay-certs/${name}.cert")"
    if [[ ! -f "${root}/config/relay-certs/${name}.cert" ]] || ! [[ "${text}" == pending || "${text}" =~ ^[A-Za-z0-9+/]+=*$ ]]; then
      echo "self-test: config/relay-certs/${name}.cert is missing, or neither a certificate's base64 nor pending" >&2
      failures=$((failures + 1))
    fi
  done
  rm -rf "${dir}"
  [[ ${failures} -eq 0 ]] && echo "check_relay_ids self-test: ok"
  [[ ${failures} -eq 0 ]]
}

case "${1:-}" in
  release | dogfood) check "$1" ;;
  --self-test) self_test ;;
  *)
    echo "usage: $0 release|dogfood|--self-test" >&2
    exit 2
    ;;
esac
