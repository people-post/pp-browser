# Privacy — phases

## Y1 — Direct audience (T1, pp-browser)

- [ ] Setting `mesh.direct_connections` (everyone / contacts / friendly / nobody; default contacts) + `AddressDisclosureGate` published by the hub from contacts
- [ ] Punch target answers only allowed initiators
- [ ] Calls: a peer that is not allowed gets a relay-only path (no direct dial, no punch, no k3 upgrade) and no listen addresses in call signalling
- [ ] Chat: no new direct Amp dial to a peer that is not allowed (Brief relay fallback)
- [ ] LAN discovery dials follow the audience
- [ ] Me → Network setting; docs promoted (CONFIGURATION, CALLS / NETWORKING)

## Y2 — Addresses on links and in records (T1, pp-cpp-amp)

- [ ] ch0 capability carries listen addresses only to allowed peers (per-peer filter in Amp)
- [ ] Clients register without IP addresses; the directory points at their rendezvous relays

## Y3 — Inbound control (T3)

- [ ] Blocked peers refused at the link (after the handshake identifies them)
- [ ] Who can message / call / add me

## Y4 — Relay trust (T4)

- [ ] Strict mode: relays limited to org seeds and trusted contacts' Nodes

## Later

- T2 server metadata audit and minimisation
- T5 presence (online / live / read state audiences)
- T6 log and diagnostics redaction audit
