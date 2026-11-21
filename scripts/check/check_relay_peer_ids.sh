#!/usr/bin/env bash
# The org relays' PeerIds a build ships beside their origins (multi-relay MR008: a relay address carries its PeerId;
# the app never learns it from the relay's own answer). Reads PP_API_URL_PROD / _STAGING / _DEV and
# PP_RELAY_PEER_ID_PROD / _STAGING / _DEV from the environment; the deprecated PP_API_URL_QA /
# PP_RELAY_PEER_ID_QA stand in for staging while its own variable is empty.
#
#   check_relay_peer_ids.sh release   a release (tagged installers, TestFlight, a published pp-node): fails when
#                                     PP_RELAY_PEER_ID_PROD is empty, or a staging / dev origin is set
#                                     without its PeerId, or a PeerId is malformed
#   check_relay_peer_ids.sh dogfood   warns instead (the build pins the org relay on its first answer)
#   check_relay_peer_ids.sh --self-test
set -euo pipefail

peer_id_form() {
  # base58btc of a SHA-256 multihash: `Qm` and 44 more characters of the Bitcoin alphabet.
  [[ "$1" =~ ^Qm[1-9A-HJ-NP-Za-km-z]{44}$ ]]
}

check_env() { # check_env <label> <origin> <peer id>
  if [[ -n "$2" && -z "$3" ]]; then
    problems+=("PP_RELAY_PEER_ID_$1 is empty while its origin is set: the $1 relay would be pinned on its first answer")
  elif [[ -n "$3" ]] && ! peer_id_form "$3"; then
    problems+=("PP_RELAY_PEER_ID_$1 is not a PeerId (Qm…, 46 characters): $3")
  fi
}

check() {
  local mode="$1" problems=()
  local prod="${PP_RELAY_PEER_ID_PROD:-}"
  if [[ -z "${prod}" ]]; then
    problems+=("PP_RELAY_PEER_ID_PROD is empty: the prod relay would be pinned on its first answer")
  elif ! peer_id_form "${prod}"; then
    problems+=("PP_RELAY_PEER_ID_PROD is not a PeerId (Qm…, 46 characters): ${prod}")
  fi
  check_env STAGING "${PP_API_URL_STAGING:-${PP_API_URL_QA:-}}" "${PP_RELAY_PEER_ID_STAGING:-${PP_RELAY_PEER_ID_QA:-}}"
  check_env DEV "${PP_API_URL_DEV:-}" "${PP_RELAY_PEER_ID_DEV:-}"
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
  local good="QmTWkSQAcGsETogTFKrPJ3GzdHG3vP9UwXVyp2HeK9YBvP" failures=0
  expect() { # expect <0|1> <mode> <prod> <staging url> <staging peer> [<dev url> <dev peer> [<qa url> <qa peer>]]
    local want="$1" got=0
    PP_RELAY_PEER_ID_PROD="$3" PP_API_URL_STAGING="$4" PP_RELAY_PEER_ID_STAGING="$5" \
      PP_API_URL_DEV="${6:-}" PP_RELAY_PEER_ID_DEV="${7:-}" PP_API_URL_QA="${8:-}" PP_RELAY_PEER_ID_QA="${9:-}" \
      check "$2" >/dev/null || got=1
    if [[ "${got}" != "${want}" ]]; then
      echo "self-test: check $2 ($*) exited ${got}, wanted ${want}" >&2
      failures=$((failures + 1))
    fi
  }
  expect 0 release "${good}" "" ""
  expect 1 release "" "" ""
  expect 1 release "QmShort" "" ""
  expect 1 release "${good}" "https://staging.example.test" ""
  expect 0 release "${good}" "https://staging.example.test" "${good}"
  expect 1 release "${good}" "" "not-a-peer-id"
  expect 1 release "${good}" "" "" "https://dev.example.test" ""
  expect 0 release "${good}" "" "" "https://dev.example.test" "${good}"
  expect 1 release "${good}" "" "" "" "not-a-peer-id"
  expect 1 release "${good}" "https://staging.example.test" "${good}" "https://dev.example.test" ""
  expect 1 release "${good}" "" "" "" "" "https://qa.example.test" ""
  expect 0 release "${good}" "" "" "" "" "https://qa.example.test" "${good}"
  expect 1 release "${good}" "" "" "" "" "" "not-a-peer-id"
  expect 0 dogfood "" "https://staging.example.test" ""
  [[ ${failures} -eq 0 ]] && echo "check_relay_peer_ids self-test: ok"
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
