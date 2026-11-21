#!/usr/bin/env bash
# The org relays' ids and certificates a build ships beside their origins (release-compatibility SIGNED_FORMATS § 3.3,
# § 3.5: a relay address is its origin and its relay id; the build's certificate is the floor a fresh install starts
# from). Reads PP_API_URL_QA, PP_RELAY_ID_PROD / PP_RELAY_ID_QA from the environment, and the certificate files
# config/relay-certs/prod.cert / qa.cert (PP_RELAY_CERT_FILE_PROD / _QA override the paths).
#
# An unset or empty PP_RELAY_ID_<ENV> (a CI variable not set reads as empty) is what the build scripts pass as the
# build's default, "pending": that environment's operator set is not made yet, and the relay is not used (it fails
# closed). Only a CMake `-DPP_RELAY_ID_<ENV>=` given by hand builds a first-use (dogfood) relay.
#
#   check_relay_ids.sh release   a release (tagged installers, TestFlight, a published pp-node): fails when
#                                PP_RELAY_ID_PROD is not a relay id (empty, pending, malformed) or prod.cert holds no
#                                certificate; when PP_API_URL_QA is set, the same for PP_RELAY_ID_QA and qa.cert
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
  done < <(check_env PROD "${PP_RELAY_ID_PROD:-pending}" "${PP_RELAY_CERT_FILE_PROD:-${root}/config/relay-certs/prod.cert}")
  if [[ -n "${PP_API_URL_QA:-}" ]]; then
    while IFS= read -r line; do
      [[ -n "${line}" ]] && problems+=("${line}")
    done < <(check_env QA "${PP_RELAY_ID_QA:-pending}" "${PP_RELAY_CERT_FILE_QA:-${root}/config/relay-certs/qa.cert}")
  fi
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
  expect() { # expect <0|1> <mode> <prod id|UNSET> <prod cert> <qa url> <qa id|UNSET> <qa cert>
    local want="$1" got=0
    (
      [[ "$3" == UNSET ]] && unset PP_RELAY_ID_PROD || export PP_RELAY_ID_PROD="$3"
      [[ "$6" == UNSET ]] && unset PP_RELAY_ID_QA || export PP_RELAY_ID_QA="$6"
      PP_RELAY_CERT_FILE_PROD="${dir}/$4" PP_API_URL_QA="$5" PP_RELAY_CERT_FILE_QA="${dir}/$7" check "$2"
    ) >/dev/null || got=1
    if [[ "${got}" != "${want}" ]]; then
      echo "self-test: check $2 prod='$3' ($4) qa_url='$5' qa='$6' ($7) exited ${got}, wanted ${want}" >&2
      failures=$((failures + 1))
    fi
  }
  expect 0 release "${good}" good.cert "" UNSET pending.cert
  expect 1 release UNSET good.cert "" UNSET pending.cert             # the placeholder fails closed
  expect 1 release pending good.cert "" UNSET pending.cert
  expect 1 release "" good.cert "" UNSET pending.cert                # empty: what CI passes, pending
  expect 1 release "relay:short" good.cert "" UNSET pending.cert
  expect 1 release "QmTWkSQAcGsETogTFKrPJ3GzdHG3vP9UwXVyp2HeK9YBvP" good.cert "" UNSET pending.cert
  expect 1 release "relay:AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh9" good.cert "" UNSET pending.cert # spare bits
  expect 1 release "${good}" pending.cert "" UNSET pending.cert      # no certificate floor
  expect 1 release "${good}" bad.cert "" UNSET pending.cert
  expect 1 release "${good}" missing.cert "" UNSET pending.cert
  expect 1 release "${good}" good.cert "https://qa.example.test" UNSET good.cert
  expect 1 release "${good}" good.cert "https://qa.example.test" "${good}" pending.cert
  expect 0 release "${good}" good.cert "https://qa.example.test" "${good}" good.cert
  expect 0 dogfood UNSET pending.cert "https://qa.example.test" "" pending.cert
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
