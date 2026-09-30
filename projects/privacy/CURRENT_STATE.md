# Privacy — current state

**As of:** 2026-09-30
**Branch:** `feat/privacy`

**Y1 in progress** (direct audience). Nothing shipped yet.

Exposure map at start (2026-09-30): the punch target answers any initiator; ch0 sends every linked peer our listen addresses (LAN included); client registration publishes addresses to the directory; calls dial direct first and send listen addresses in signalling; chat dials direct to any reachable peer; `TrustLevel::Friendly` has no behavioural use; Blocked is not enforced at the link.
