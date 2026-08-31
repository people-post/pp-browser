# Runtime image for org/datacenter pp-node (N011).
#
# glibc contract: same OS family as release CI — Ubuntu 24.04 (glibc 2.39).
# Build the binary on ubuntu-24.04 (see scripts/pp_node_package_linux.sh);
# do not mix Debian bookworm binaries into this image or vice versa.
#
# Build context is a staging directory with:
#   pp-node              (stripped Release binary)
#   config.json.example
# See scripts/pp_node_package_linux.sh and .github/workflows/release.yml.

FROM ubuntu:24.04

RUN apt-get update && apt-get install -y --no-install-recommends \
      ca-certificates \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --system --uid 101 --home /var/lib/pp-node --create-home pp-node \
    && mkdir -p /etc/pp-node \
    && chown pp-node:pp-node /etc/pp-node

COPY --chmod=755 pp-node /usr/local/bin/pp-node
COPY --chown=pp-node:pp-node config.json.example /etc/pp-node/config.json

USER pp-node
WORKDIR /var/lib/pp-node
VOLUME ["/var/lib/pp-node"]

# Peer-facing Amp UDP listen (often 443/udp). Binding privileged ports needs
# CAP_NET_BIND_SERVICE (Docker: --cap-add=NET_BIND_SERVICE;
# Kubernetes: securityContext.capabilities.add: ["NET_BIND_SERVICE"]).
# Status HTTP is separate (default loopback 18518). For console/probes:
#   -e PP_NODE_STATUS_ADDR=0.0.0.0:18518 -p 18518:18518
#   -e PP_NODE_AMP_UDP_PORT=18517 -p 18517:18517/udp
EXPOSE 443/udp
EXPOSE 18518

# Provide secrets and deploy overlays via env — never bake a PIN into the image.
# Precedence: CLI → env → this config file → defaults. See docs/ops/CONFIGURATION.md.
#   PP_BROWSER_PIN              (required)
#   PP_NODE_AMP_UDP_PORT        e.g. 443 (org seed) or 18517 (local compose)
#   PP_NODE_DATA_DIR            e.g. /var/lib/pp-node
#   PP_NODE_BOOTSTRAP_PEERS     comma-separated Amp ADP multiaddrs
#   PP_NODE_CAP_CIRCUIT_RELAY / PP_NODE_CAP_MEDIA_RELAY
#   PP_NODE_STATUS_ADDR         e.g. 0.0.0.0:18518 for console
#   PP_NODE_STATUS_TOKEN        (optional Bearer for status HTTP)
ENTRYPOINT ["/usr/local/bin/pp-node", "--config", "/etc/pp-node/config.json"]
