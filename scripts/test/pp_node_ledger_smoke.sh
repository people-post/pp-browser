#!/usr/bin/env bash
# N-LEDGER (pp-ledger projects/platform-integration pi4): pp-node runs a pp-ledger role on its mesh
# runtime, with the node's identity and address.
#
#   relay  standalone beacon → pp-node relay → standalone miner: the tip advances after a
#          transaction inject, and the pp-node relay serves pp-client.
#   miner  standalone beacon → standalone relay → pp-node miner: the tip advances, and the pp-node
#          miner serves pp-client.
#
# The standalone nodes, genesis and transactions come from pp-ledger's own smoke library, so this
# needs a pp-ledger checkout with its apps built (pp-beacon, pp-relay, pp-miner, pp-client):
#   PP_LEDGER_ROOT       pp-ledger checkout (default: ../pp-ledger beside this repo)
#   PP_LEDGER_BUILD_DIR  its build directory (default: $PP_LEDGER_ROOT/build)
#   PP_LOCAL_BUILD_DIR   this repo's build with pp-node (default: ./build)
#
# Usage:
#   ./scripts/test/pp_node_ledger_smoke.sh [relay|miner|all]
set -euo pipefail

PB_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PP_LEDGER_ROOT="${PP_LEDGER_ROOT:-${PB_ROOT}/../pp-ledger}"
PP_NODE_BIN="${PP_LOCAL_BUILD_DIR:-${PB_ROOT}/build}/src/app/node/pp-node"
SCENARIO="${1:-all}"

[[ -f "${PP_LEDGER_ROOT}/scripts/test/pp_ledger_smoke_lib.sh" ]] ||
  { echo "pp-ledger checkout not found at ${PP_LEDGER_ROOT} (set PP_LEDGER_ROOT)" >&2; exit 1; }
[[ -x "$PP_NODE_BIN" ]] || { echo "pp-node not found at ${PP_NODE_BIN} (set PP_LOCAL_BUILD_DIR)" >&2; exit 1; }

# One miner; a test directory of its own, beside pp-ledger's smoke.
export PP_LEDGER_SMOKE_MINERS=1
export PP_LEDGER_BUILD_DIR="${PP_LEDGER_BUILD_DIR:-${PP_LEDGER_ROOT}/build}"
export PP_LEDGER_SMOKE_DIR="${PP_LEDGER_SMOKE_DIR:-${PP_LEDGER_BUILD_DIR}/test-node-ledger}"
# shellcheck source=/dev/null
source "${PP_LEDGER_ROOT}/scripts/test/pp_ledger_smoke_lib.sh"

readonly NODE_PIN="246810"
readonly NODE_UDP_PORT="${PP_NODE_LEDGER_SMOKE_UDP_PORT:-18717}"
readonly TIP_TIMEOUT="${PP_NODE_LEDGER_SMOKE_TIP_TIMEOUT_SEC:-180}"

# pp-node as ledger node <name> (relay / miner1): role $2 on work directory ${TEST_DIR}/<name>, its
# own data directory beside it; registered with the network's PIDs and multiaddrs under <name>.
start_pp_node() {
  local name=$1 role=$2
  local work_dir="${TEST_DIR}/${name}" data_dir="${TEST_DIR}/${name}-pp-node"
  mkdir -p "$data_dir"
  # A config of its own: never the developer's (its mesh settings, its bootstrap peers).
  echo '{}' >"${data_dir}/config.json"
  # pp-ledger takes a non-empty work directory only with its signature (as launch_miner does).
  [[ -f "${work_dir}/.signature" ]] || touch "${work_dir}/.signature"
  PP_NODE_DATA_DIR="$data_dir" PP_NODE_AMP_UDP_PORT="$NODE_UDP_PORT" PP_NODE_MESH_PUBLISH=0 \
    run_bg_cmd "${work_dir}/console.log" "$PP_NODE_BIN" --config "${data_dir}/config.json" --pin "$NODE_PIN" \
    --status-addr "" ${DEBUG_FLAG} \
    --ledger-role "$role" --ledger-dir "$work_dir"
  save_pid "$name" $!
  wait_for_listen_multiaddr "${work_dir}/console.log" "${MULTIADDR_DIR}/${name}" 60 ||
    { stop_network; die "pp-node ${role}: no ledger listener"; }
  # pp-node binds dual-stack: dial its wildcard by name, as the library does for /ip4/0.0.0.0.
  sed -i 's|^/ip6/::/|/dns4/localhost/|' "${MULTIADDR_DIR}/${name}"
  wait_until_ready "$name" "${work_dir}/console.log" 90 || { stop_network; die "pp-node ${role} not ready"; }
  grep -aq "on the host's Amp runtime" "${work_dir}/console.log" ||
    { stop_network; die "pp-node ${role} did not run on the node's mesh runtime"; }
  echo -e "${GREEN}✓ pp-node ${role} on UDP ${NODE_UDP_PORT}${NC}"
}

fresh_network() {
  stop_network || true
  rm -rf "$TEST_DIR"
  mkdir -p "$TEST_DIR" "$MULTIADDR_DIR"
  initialize_beacon_with_test_config
  create_beacon_config
  start_beacon
}

# The tip moves through the topology after an inject, and node $1's ledger serves pp-client.
assert_blocks_flow() {
  local served_by=$1 start_tip end_tip reply
  wait_for_beacon_rpc 60
  start_tip=$(get_next_block_id)
  inject_transactions_for_block_production
  wait_for_tip_increase "$start_tip" "$TIP_TIMEOUT" || die "tip did not advance past ${start_tip} in ${TIP_TIMEOUT}s"
  end_tip=$(get_next_block_id)
  reply=$(run_pp_client -b --host "$(cat "${MULTIADDR_DIR}/${served_by}")" health 2>&1) ||
    die "pp-client health via ${served_by} failed: ${reply}"
  echo -e "${GREEN}✓ nextBlockId ${start_tip} → ${end_tip}; ${served_by} serves pp-client${NC}"
}

relay_scenario() {
  echo -e "${BLUE}═══ N-LEDGER relay: beacon → pp-node relay → miner ═══${NC}"
  fresh_network
  mkdir -p "${TEST_DIR}/relay"
  create_relay_config relay "$RELAY_PORT"  # its port is unused: the node's mesh carries it
  start_pp_node relay relay
  start_all_miners
  assert_blocks_flow relay
  stop_network
  echo -e "${GREEN}N-LEDGER relay PASSED${NC}"
}

miner_scenario() {
  echo -e "${BLUE}═══ N-LEDGER miner: beacon → relay → pp-node miner ═══${NC}"
  fresh_network
  start_relay
  mkdir -p "${TEST_DIR}/miner1"
  create_miner_config 1 "${TEST_DIR}/miner1" "$MINER_BASE_PORT"
  start_pp_node miner1 miner
  assert_blocks_flow miner1
  stop_network
  echo -e "${GREEN}N-LEDGER miner PASSED${NC}"
}

verify_build
trap 'stop_network || true' EXIT
case "$SCENARIO" in
  relay) relay_scenario ;;
  miner) miner_scenario ;;
  all) relay_scenario; miner_scenario ;;
  *) die "unknown scenario: ${SCENARIO} (relay|miner|all)" ;;
esac
echo -e "${GREEN}N-LEDGER PASSED (${SCENARIO})${NC}"
