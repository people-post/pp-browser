# Node relay — phases

Ordering only; status lives in [CURRENT_STATE.md](CURRENT_STATE.md). Design: [DESIGN.md](DESIGN.md).

- [ ] **NR0** Design: this folder (NR001–NR011 decided); the privacy statement with the privacy project
- [ ] **NR1** The protocol in pp-specs (capability-repos CR025 step 2): the relay calls apart from transport, the HTTPS
      binding as it is, the Amp binding's frames, the vectors; certificate version 2 and the card's `amp_relays` and
      `delivery` entries; the invite, the revoke, the offer and both halves of ownership (NR008, NR011); their vectors
- [ ] **NR2** The edition: the `relay-services` capability (empty), `pp-node-relay` built beside `pp-node` with its own
      image; CI checks that the default `pp-node` does not link it, and the default image's size
- [ ] **NR3** The mailbox on `pp-node-relay`: registration, send, poll, ack, clear and `watch` over `/pp/rpc/relay/1.0.0`;
      certificate v2 and `relay-identity` over Amp; admission by invite, the owner's revoke and the collect-only
      period (NR008); a small store with NR007's limits, each a setting,
      reported in `relay-identity`;
      `/metrics` and the load level; the relay protocol's server checks against pp-specs' vectors
- [ ] **NR4** The app: the Amp binding (a client per relay, `watch` while connected); `amp_relays` and `delivery` in our card;
      adding a node relay in My relays (by its link or QR code), and the delivery choice; owning a relay
      (the card's half, the offer); "Invite to my relay" on a contact's page, the invite sweep, revoke on remove or block; sending by NR009 and NR006;
      polling every relay
- [ ] **NR5** The app lab (a home group with the org relay down; a full node relay; a restart; two devices), a
      `pp-node-relay` release, and the Pi as the first node relay
- [ ] **NR6** The push gateway; then Amp-only accounts (NR006's last sentence); the `files` service on node relays (NR010);
      lockers and cards on node relays; later, content-addressed attachment references
