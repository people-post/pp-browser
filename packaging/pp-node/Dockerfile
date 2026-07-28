# Sketch image for org/datacenter pp-node (N011). Not a production hardening guide.
FROM debian:bookworm-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
      ca-certificates \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --system --home /var/lib/pp-node --create-home pp-node

COPY pp-node /usr/local/bin/pp-node
COPY config.json.example /etc/pp-node/config.json

USER pp-node
WORKDIR /var/lib/pp-node
EXPOSE 443

# Provide PIN via env or orchestration secrets:
# ENV PP_BROWSER_PIN=...
ENTRYPOINT ["/usr/local/bin/pp-node", "--config", "/etc/pp-node/config.json"]
