# Privacy — phases

## Y1 — Direct audience (T1, pp-browser)

- [x] Setting `mesh.direct_connections` (everyone / contacts / friendly / nobody; default contacts) + `AddressDisclosureGate` published by the hub from contacts
- [x] Punch target answers only allowed initiators
- [x] Calls: a peer that is not allowed gets a relay-only path (no direct dial, no punch, no k3 upgrade), no listen addresses in call signalling, no signalling-punch answer
- [x] Chat, attachments, history: no new direct Amp dial to a peer that is not allowed (existing link, else Brief relay)
- [x] LAN discovery: only books addresses; the dials go through the gated paths above
- [x] Me → Network setting (+ settings assistant); docs promoted (CONFIGURATION, NETWORKING, CALLS)
- [x] Hard-lab scenario B-HARD-CALL-NAT-PRIVATE: a stranger's call to a `contacts` peer runs relay-only (its punches declined) and still connects
- [x] Peer-announce / broadcast fetches from a stranger publisher: the viewer's choice, not gated ([P005](DECISIONS.md#p005--watching-a-strangers-broadcast-is-the-viewers-choice))

## Y2 — Addresses on links and in records (T1, pp-cpp-amp)

- [x] ch0 capability carries listen addresses only to allowed peers (pp-cpp-amp v2.13.0 `SetListenAddrDisclosure`; `MeshHost` applies the gate)
- [x] Clients register without IP addresses unless their audience is everyone (PeerId only; peers reach them through relays)
- [ ] Re-register on an audience change (today: the next registration drops old addresses)

## Y3 — Inbound control (T3)

- [x] Blocked enforced inbound: direct messages and call control discarded, links dropped on connect ([P006](DECISIONS.md#p006--blocked-means-no-contact-at-all))
- [x] Who can call me: `call_invite_policy` (everyone / contacts_only / nobody), silent drop ([P007](DECISIONS.md#p007--who-can-call-me-strangers-messages-stay-open-for-now)); who can add me = the existing group-invite policy
- [ ] Group messages from a Blocked member
- [ ] Message requests: strangers' first messages held for the user to accept

## Y4 — Relay trust (T4)

- [ ] Strict mode: relays limited to org seeds and trusted contacts' Nodes

## Later

- LAN: a phone's in-call mDNS advertisement shows its LAN address to the LAN (T1, low risk)

- T2 server metadata audit and minimisation
- T5 presence (online / live / read state audiences)
- T6 log and diagnostics redaction audit
