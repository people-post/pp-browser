# Privacy — current state

**As of:** 2026-09-30
**Branch:** `feat/privacy`

**Y1 in progress** (direct audience) on `feat/privacy`: setting + gate, punch target, calls (relay-only, signalling), chat / attachments / history (`DisclosureGatedPeerLinks`), Me → Network. Y1 done (hard lab: B-HARD-CALL-NAT-PRIVATE; peer-announce is the viewer's choice, P005). Y2: ch0 capability filtered (pp-cpp-amp v2.13.0); open: client registration without IP. Behaviour: [NETWORKING § Address disclosure](../../docs/architecture/NETWORKING.md#address-disclosure-privacy).

Exposure map at start (2026-09-30): the punch target answers any initiator; ch0 sends every linked peer our listen addresses (LAN included); client registration publishes addresses to the directory; calls dial direct first and send listen addresses in signalling; chat dials direct to any reachable peer; `TrustLevel::Friendly` has no behavioural use; Blocked is not enforced at the link.
