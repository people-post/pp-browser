#!/bin/sh
# Configure SNAT gateway for hard-lab CGNAT-ish topo.
# Env:
#   PP_HARD_GW_PRIV_CIDR  e.g. 10.117.1.0/24  (iface with this addr = private)
#   PP_HARD_GW_PUB_CIDR   e.g. 198.18.117.0/24  (iface with this addr = public)
#   PP_HARD_GW_NAT        symmetric (default) | cone
#     symmetric — MASQUERADE --random-fully: a fresh public port per destination, so the port the
#                 hop observes is useless to the other peer and hole punching can never land.
#     cone      — port-preserving MASQUERADE (endpoint-independent mapping): punchable.
#   Scripts flip the mode at runtime with pp_hard_cgnat_set_nat (scripts/test/pp_hard_lab_lib.sh).
set -eu

priv_cidr="${PP_HARD_GW_PRIV_CIDR:?PP_HARD_GW_PRIV_CIDR required}"
pub_cidr="${PP_HARD_GW_PUB_CIDR:?PP_HARD_GW_PUB_CIDR required}"
nat_mode="${PP_HARD_GW_NAT:-symmetric}"
case "${nat_mode}" in
  symmetric) masq_extra="--random-fully" ;;
  cone) masq_extra="" ;;
  *) echo "error: PP_HARD_GW_NAT must be symmetric|cone (got ${nat_mode})" >&2; exit 1 ;;
esac

iface_for_cidr() {
  cidr="$1"
  prefix="$(echo "${cidr}" | cut -d/ -f1 | cut -d. -f1-3)"
  ip -o -4 addr show | awk -v p="${prefix}." '
    index($0, p) {
      split($2, a, "@");
      print a[1];
      exit
    }'
}

priv_if="$(iface_for_cidr "${priv_cidr}")"
pub_if="$(iface_for_cidr "${pub_cidr}")"
[ -n "${priv_if}" ] || { echo "error: no iface for priv ${priv_cidr}" >&2; exit 1; }
[ -n "${pub_if}" ] || { echo "error: no iface for pub ${pub_cidr}" >&2; exit 1; }
[ "${priv_if}" != "${pub_if}" ] || { echo "error: priv/pub iface collide (${priv_if})" >&2; exit 1; }

# Prefer compose sysctls (net.ipv4.ip_forward=1); /proc may be read-only.
if [ -w /proc/sys/net/ipv4/ip_forward ]; then
  echo 1 >/proc/sys/net/ipv4/ip_forward
elif [ "$(cat /proc/sys/net/ipv4/ip_forward 2>/dev/null || echo 0)" != "1" ]; then
  echo "error: ip_forward=0 and /proc not writable; set sysctls net.ipv4.ip_forward=1 on the gw service" >&2
  exit 1
fi

# Default deny forward; allow private→public and return traffic only.
iptables -F FORWARD
iptables -F INPUT
iptables -t nat -F POSTROUTING
iptables -P FORWARD DROP
iptables -A FORWARD -m conntrack --ctstate ESTABLISHED,RELATED -j ACCEPT
iptables -A FORWARD -i "${priv_if}" -o "${pub_if}" -j ACCEPT
# Explicitly drop unsolicited public→private (defense in depth; no DNAT anyway).
iptables -A FORWARD -i "${pub_if}" -o "${priv_if}" -m conntrack --ctstate NEW -j DROP
# Firewall the gateway itself like a real router: unsolicited WAN packets are dropped before
# conntrack confirms them. Accepting them (the kernel default) records a flow for a peer's early
# punch packet, which then clashes with our own host's outbound mapping and makes MASQUERADE pick
# another port — punches fail for a reason no real NAT has.
iptables -A INPUT -i "${pub_if}" -m conntrack --ctstate NEW -j DROP

# shellcheck disable=SC2086
iptables -t nat -A POSTROUTING -s "${priv_cidr}" -o "${pub_if}" -j MASQUERADE ${masq_extra}

echo "hard-gw ready priv_if=${priv_if} (${priv_cidr}) pub_if=${pub_if} (${pub_cidr}) MASQUERADE nat=${nat_mode}"

exec sleep infinity
