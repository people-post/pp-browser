#!/usr/bin/env bash
# The org relays' ids and certificates a build ships beside their origins (release-compatibility SIGNED_FORMATS § 3.3,
# § 3.5: a relay address is its origin and its relay id; the build's certificate is the floor a fresh install starts
# from). Reads PP_API_URL_STAGING / PP_API_URL_DEV and PP_RELAY_ID_PROD / _STAGING / _DEV from the environment, and
# the certificate files config/relay-certs/prod.cert / staging.cert / dev.cert (PP_RELAY_CERT_FILE_PROD / _STAGING /
# _DEV override the paths). The environments are dev, staging and prod (release-compatibility RC009, RC021).
#
# A tester build (PP_TESTER_BUILD=ON, release-compatibility RC022: an internal TestFlight upload) compiles no prod: prod
# is not checked, and a set PP_API_URL_PROD, or a PP_RELAY_ID_PROD other than empty or pending, fails in every mode (CMake
# would refuse that build).
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
#
# In both modes a certificate of another relay id fails: a relay id and a certificate that are both set but disagree
# are never intended (the file and the variable were not changed together), and the app would drop that floor with
# only a log warning. The check reads the founding operator set from the certificate and hashes it to its relay id
# (SIGNED_FORMATS § 3.1); it does not verify signatures (the app does, and pp-relay-apis' `relay-operator.mjs verify`).
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

# The relay id a certificate is of (SIGNED_FORMATS § 3.1): BLAKE2b-256("pp-browser:relay-id" 00 ‖ u8 1 ‖ founding
# set), the founding set being the first LenBytes (u64 big-endian length) of the body. The blob must also end where
# its signatures do (a copy cut short). Prints the id, or prints a reason and exits 1 when the blob is not a version-1
# certificate this check can read.
certificate_relay_id() { # certificate_relay_id <certificate base64>
  python3 -c '
import base64, binascii, hashlib, struct, sys

def fail(why):
    print(why)
    sys.exit(1)

try:
    blob = base64.b64decode(sys.argv[1], validate=True)
except (binascii.Error, ValueError):
    fail("not base64")
if len(blob) < 1 or blob[0] != 1:
    fail("certificate version %s, this build reads 1" % (blob[0] if blob else "missing"))

def len_bytes(data, at):
    if len(data) < at + 8:
        fail("truncated")
    (n,) = struct.unpack(">Q", data[at : at + 8])
    if len(data) < at + 8 + n:
        fail("truncated")
    return data[at + 8 : at + 8 + n]

body = len_bytes(blob, 1)
at = 9 + len(body)
if len(blob) < at + 1:
    fail("truncated")
signatures, at = blob[at], at + 1
for _ in range(signatures):
    at += 1 + 8 + len(len_bytes(blob, at + 1))
if at != len(blob):
    fail("truncated" if at > len(blob) else "bytes after the signatures")
founding = len_bytes(body, 0)
if len(founding) < 2 or not 1 <= founding[0] <= founding[1] <= 8:
    fail("no operator set (threshold and size outside 1 <= t <= n <= 8)")
digest = hashlib.blake2b(b"pp-browser:relay-id\x00\x01" + founding, digest_size=32).digest()
print("relay:" + base64.urlsafe_b64encode(digest).decode().rstrip("="))
' "$1"
}

check_env() { # check_env <ENV> <relay id> <certificate file>; prints problems, "fatal: " first when every mode fails
  local env="$1" id="$2" file="$3" cert of
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
  elif ! of="$(certificate_relay_id "${cert}")"; then
    echo "fatal: ${file#"${root}/"} is not a certificate this build reads: ${of}"
  elif relay_id_form "${id}" && [[ "${of}" != "${id}" ]]; then
    echo "fatal: ${file#"${root}/"} is the certificate of ${of}, not of PP_RELAY_ID_${env} (${id}): a new operator set is a new relay id, so the file and the variable change together"
  fi
}

check() {
  local mode="$1" problems=() line
  while IFS= read -r line; do
    [[ -n "${line}" ]] && problems+=("${line}")
  done < <(
    if [[ "$(printf '%s' "${PP_TESTER_BUILD:-OFF}" | tr '[:lower:]' '[:upper:]')" == ON ]]; then
      # A tester build has no prod (RC022): nothing of prod's to check, and nothing of prod's may be passed.
      if [[ -n "${PP_API_URL_PROD:-}" ]]; then
        echo "fatal: PP_TESTER_BUILD is ON but PP_API_URL_PROD is set: a tester build compiles no prod origin"
      fi
      if [[ -n "${PP_RELAY_ID_PROD:-}" && "${PP_RELAY_ID_PROD}" != "pending" ]]; then
        echo "fatal: PP_TESTER_BUILD is ON but PP_RELAY_ID_PROD is set: a tester build compiles no prod relay id"
      fi
    else
      check_env PROD "${PP_RELAY_ID_PROD:-pending}" "${PP_RELAY_CERT_FILE_PROD:-${root}/config/relay-certs/prod.cert}"
    fi
    # Staging and dev only where the build compiles their origin (a store build ships prod only, RC015).
    if [[ -n "${PP_API_URL_STAGING:-}" ]]; then
      check_env STAGING "${PP_RELAY_ID_STAGING:-pending}" \
        "${PP_RELAY_CERT_FILE_STAGING:-${root}/config/relay-certs/staging.cert}"
    fi
    if [[ -n "${PP_API_URL_DEV:-}" ]]; then
      check_env DEV "${PP_RELAY_ID_DEV:-pending}" "${PP_RELAY_CERT_FILE_DEV:-${root}/config/relay-certs/dev.cert}"
    fi
  )
  local problem fatal=0
  for problem in "${problems[@]+"${problems[@]}"}"; do
    if [[ "${problem}" == fatal:* ]]; then
      echo "::error::${problem#fatal: }"
      fatal=1
    elif [[ "${mode}" == "release" ]]; then
      echo "::error::${problem}"
    else
      echo "::warning::${problem}"
    fi
  done
  [[ ${fatal} -eq 0 && ( "${mode}" != "release" || ${#problems[@]} -eq 0 ) ]]
}

# pp-specs' vectors/ folder (the vectors live there, at the tag cmake/PpPins.cmake pins): PP_SPECS_DIR, else a sibling
# checkout (../pp-specs), else a build's fetched copy (build*/_deps/pp_specs-src). CI's lint job sets PP_SPECS_DIR
# (scripts/dev/pp_specs_at_pin.sh).
pp_specs_vectors() {
  local d
  for d in "${PP_SPECS_DIR:-}" "${root}/../pp-specs" "${root}"/build*/_deps/pp_specs-src; do
    if [[ -n "${d}" && -f "${d}/vectors/manifest.json" ]]; then
      echo "${d}/vectors"
      return 0
    fi
  done
  echo "pp-specs not found: set PP_SPECS_DIR (scripts/dev/pp_specs_at_pin.sh <dir> clones the pinned tag)" >&2
  return 1
}

self_test() {
  # Real certificates from the frozen vectors (SIGNED_FORMATS § 6): certificate 1 of the vectors' relay, wrapped and
  # commented as a .cert file is; another relay's; a version this build does not read; a truncated one.
  local good="relay:lelZMZgWuhoMQTvLLUT1C5qlJBPX66oxOLzIT_VsEs4" failures=0 dir
  local vectors
  vectors="$(pp_specs_vectors)/relay_certificate_vectors.json" || return 1
  dir="$(mktemp -d)"
  python3 - "${vectors}" "${dir}" <<'PY_VECTORS'
import json, sys, textwrap

cases = {c["name"]: c for c in json.load(open(sys.argv[1]))["cases"]}
for file, name in [("good", "certificate_1"), ("other", "other_relay_id"), ("version2", "version_2"),
                   ("truncated", "truncated")]:
    with open("%s/%s.cert" % (sys.argv[2], file), "w") as out:
        out.write("# a comment\n" + "\n".join(textwrap.wrap(cases[name]["certificate_b64"], 120)) + "\n")
PY_VECTORS
  printf '# placeholder\npending\n' >"${dir}/pending.cert"
  printf 'not base64!\n' >"${dir}/bad.cert"
  # expect <0|1> <mode> <prod id|UNSET> <prod cert> [<ENV> <url> <id|UNSET> <cert>]...: ENV is STAGING or DEV
  expect() {
    local want="$1" mode="$2" got=0 args="$*"
    (
      unset PP_RELAY_ID_PROD PP_RELAY_ID_STAGING PP_RELAY_ID_DEV PP_API_URL_PROD PP_API_URL_STAGING PP_API_URL_DEV
      export PP_TESTER_BUILD="${tester}"
      [[ -z "${prod_url}" ]] || export PP_API_URL_PROD="${prod_url}"
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
  local staging="https://staging.example.test" dev="https://dev.example.test" tester=OFF prod_url=""
  expect 0 release "${good}" good.cert
  expect 1 release UNSET good.cert             # the placeholder fails closed
  expect 1 release pending good.cert
  expect 1 release "" good.cert                # empty: what CI passes, pending
  expect 1 release "relay:short" good.cert
  expect 1 release "QmTWkSQAcGsETogTFKrPJ3GzdHG3vP9UwXVyp2HeK9YBvP" good.cert # a PeerId is no relay id
  expect 1 release "relay:lelZMZgWuhoMQTvLLUT1C5qlJBPX66oxOLzIT_VsEs5" good.cert # spare bits
  expect 1 release "${good}" pending.cert      # no certificate floor
  expect 1 release "${good}" bad.cert
  expect 1 release "${good}" missing.cert
  # A certificate of another relay id, or one this build cannot read, fails in every mode; with no id set there is
  # nothing to compare.
  expect 1 release "${good}" other.cert
  expect 1 dogfood "${good}" other.cert
  expect 0 dogfood UNSET other.cert
  expect 1 dogfood "${good}" version2.cert
  expect 1 dogfood UNSET truncated.cert
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
    expect 1 release "${good}" good.cert "${env}" "${url}" "${good}" other.cert
    expect 1 dogfood "${good}" good.cert "${env}" "${url}" "${good}" other.cert
  done
  expect 0 release "${good}" good.cert STAGING "${staging}" "${good}" good.cert DEV "${dev}" "${good}" good.cert
  expect 1 release "${good}" good.cert STAGING "${staging}" "${good}" good.cert DEV "${dev}" UNSET good.cert
  expect 1 release "${good}" good.cert STAGING "${staging}" UNSET good.cert DEV "${dev}" "${good}" good.cert
  expect 1 release UNSET good.cert STAGING "${staging}" "${good}" good.cert DEV "${dev}" "${good}" good.cert
  expect 0 dogfood UNSET pending.cert STAGING "${staging}" "" pending.cert DEV "${dev}" UNSET missing.cert
  expect 1 dogfood UNSET pending.cert STAGING "${staging}" "" pending.cert DEV "${dev}" "${good}" other.cert
  # A tester build (RC022): prod is not checked (no id, no certificate needed), and anything of prod's passed fails.
  tester=ON
  expect 0 release UNSET missing.cert STAGING "${staging}" "${good}" good.cert DEV "${dev}" "${good}" good.cert
  expect 0 release pending pending.cert STAGING "${staging}" "${good}" good.cert
  expect 0 dogfood "" missing.cert STAGING "${staging}" UNSET pending.cert DEV "${dev}" "${good}" good.cert
  expect 1 release UNSET missing.cert STAGING "${staging}" UNSET good.cert # staging still needs its own
  expect 1 dogfood "${good}" good.cert STAGING "${staging}" "${good}" good.cert # a prod relay id
  prod_url="https://prod.example.test"
  expect 1 dogfood UNSET missing.cert STAGING "${staging}" "${good}" good.cert # a prod origin
  expect 1 release UNSET missing.cert DEV "${dev}" "${good}" good.cert
  tester=on
  expect 1 dogfood UNSET missing.cert DEV "${dev}" "${good}" good.cert # the flag in any case
  tester=OFF
  expect 1 release UNSET missing.cert # a store build still needs prod's
  expect 0 release "${good}" good.cert STAGING "" UNSET pending.cert
  prod_url=""
  # The files in the tree: each environment's exists and holds a certificate this build reads or the placeholder
  # (dev's is made, 1 of 1; prod's and staging's are pending until Leo makes them).
  local name text
  for name in prod staging dev; do
    text="$(certificate_text "${root}/config/relay-certs/${name}.cert")"
    if [[ ! -f "${root}/config/relay-certs/${name}.cert" ]] ||
      { [[ "${text}" != pending ]] && ! certificate_relay_id "${text}" >/dev/null; }; then
      echo "self-test: config/relay-certs/${name}.cert is missing, or neither a certificate this build reads nor pending" >&2
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
