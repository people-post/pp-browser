# Node message relay — phases

Ordering only; status lives in [CURRENT_STATE.md](CURRENT_STATE.md). Design: [DESIGN.md](DESIGN.md).

- [ ] **NM0** Design: this folder; NM001–NM006 decided; the privacy statement drafted with the privacy project
- [ ] **NM1** The node: `/pp-browser/rpc/mailbox/1.0.0` (subscribe, deposit, fetch, ack, clear) in pp-cpp-mesh or
      pp-node, behind `capabilities.message_relay` (off by default) and the subscription policy (NM006); a bounded on-disk store
      (NM004); `/metrics` series and a load level for the service; the contract in `docs/contracts/`; frozen vectors for
      the signed requests
- [ ] **NM2** The card and My relays: the mailbox entry in the account card's extension area; adding a node mailbox (our
      own node, or by its link / QR code); subscribing and renewing from the main device
- [ ] **NM3** Sending: the deposit beside the HTTP send by the list, with the grant; node health like `RelayHealth`; never
      failing a send
- [ ] **NM4** Receiving: every device fetches from the node mailboxes beside the HTTP inbox; one merge, duplicates dropped
      by id
- [ ] **NM5** The app lab and the stress lab: a node mailbox with the HTTP relay down, a full node, a restarted node, two
      devices; then a pp-node release with it off by default, and the Pi as the first `self` mailbox
