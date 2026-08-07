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

# Peer-facing libp2p listen (often 443). Binding privileged ports needs
# CAP_NET_BIND_SERVICE (Docker: --cap-add=NET_BIND_SERVICE;
# Kubernetes: securityContext.capabilities.add: ["NET_BIND_SERVICE"]).
# Do NOT EXPOSE / publish the loopback status port (18518); query via
# docker/kubectl exec → curl http://127.0.0.1:18518/status
EXPOSE 443

# Provide secrets via env or orchestration — never bake a PIN into the image:
#   PP_BROWSER_PIN
#   PP_NODE_STATUS_TOKEN   (optional Bearer for status HTTP)
ENTRYPOINT ["/usr/local/bin/pp-node", "--config", "/etc/pp-node/config.json"]
