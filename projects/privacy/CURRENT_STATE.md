# Privacy — current state

**As of:** 2026-10-31
**Branch:** `feat/privacy` (Y1–Y4); `feat/diagnostics-upload` (P009)

**Y1 in progress** (direct audience) on `feat/privacy`: setting + gate, punch target, calls (relay-only, signalling), chat / attachments / history (`DisclosureGatedPeerLinks`), Me → Network. Y1 done (hard lab: B-HARD-CALL-NAT-PRIVATE; peer-announce is the viewer's choice, P005). Y2: ch0 capability filtered (pp-cpp-amp v2.13.0); clients register without IP unless the audience is everyone; open: re-register on an audience change. Y3: Blocked enforced inbound (messages, calls, links); Who can call me; open: Blocked in groups, message requests. Y4: trusted relays only (org seeds + Friendly contacts' nodes). Behaviour: [NETWORKING § Address disclosure](../../docs/architecture/NETWORKING.md#address-disclosure-privacy).

**Diagnostics (P009)** on `feat/diagnostics-upload`: Send diagnostics sends shareable lines only (`diag.log`, `metrics.log`, the crash record); first Diag events: network, relay HTTP / poll, send / receive failures, unlock. Secrets (P010): the PIN, recovery phrase, private keys / seed and API keys are `SecretText` — logging one does not compile. Open: more events as support asks for them; check on a device that a send goes through end to end. Contract: [DIAGNOSTICS.md](../../docs/contracts/DIAGNOSTICS.md).

**Node uptime (P011):** the pp-node directory heartbeat is opt-in (`PP_NODE_MESH_HEARTBEAT`, org seeds); the listing shows a node's state, not its last-seen time; www does not log the heartbeat's address; nodes without heartbeats never go stale. Next: N031 (signed short-lived node records over the Amp directory).

Exposure map at start (2026-09-30): the punch target answers any initiator; ch0 sends every linked peer our listen addresses (LAN included); client registration publishes addresses to the directory; calls dial direct first and send listen addresses in signalling; chat dials direct to any reachable peer; `TrustLevel::Friendly` has no behavioural use; Blocked is not enforced at the link.
