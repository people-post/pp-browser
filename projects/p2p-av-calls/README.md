# P2P A/V calls

**Status:** **a3 done** (LAN 1:1 video); **a4** gated on **blind `media_relay`** (V020/V021) — no full-mesh; NAT unclaimed  
**Owner:** Hongwei + agents  
**Stable refs:** (promote after ship) wire / wake / media-key contracts  
**Related:** [p2p-mesh](../p2p-mesh/) (n4-media blind forwarder), [group-chat](../group-chat/), [e2e-message-crypto](../e2e-message-crypto/), [push-notifications](../push-notifications/), [P2P_MESSAGING.md](../../docs/architecture/P2P_MESSAGING.md)

## One-line goal

1:1 and group **voice/video calls** over the Brief mesh — WebRTC-shaped media, mesh signaling + SFU fallback, invite-based guests (no chat-group membership required), shared media key with rotate-on-leave.

## Release scope (v1)

| In | Out |
|----|-----|
| 1:1 + group voice and video (incl. mobile) | Call recording |
| Separate `call_id` session (≠ chat thread) | Screen share |
| Invite to join (guests + late join); any group member may start | Ambient “Join” for all group members without invite |
| Hostless (ends when last participant leaves) | End-for-all host control |
| Shared call media key; rotate on leave (overlapping epochs) | Per-user media ciphertexts (chat N-key model) |
| `call_wake` push for ringing | Rich media in push payload |
| Call started / ended system hints in origin chat history | Full call transcript / recording archive |
| Soft max **16** participants (engineering may cap **8** until SFU proven) | PSTN / dial-in |

## Documents

| File | Purpose |
|------|---------|
| [DESIGN.md](DESIGN.md) | Session model, lifecycle, signaling, media, crypto, mesh deps |
| [CURRENT_STATE.md](CURRENT_STATE.md) | Codebase today |
| [PHASES.md](PHASES.md) | Delivery checklist (v0 → a6) |
| [DECISIONS.md](DECISIONS.md) | ADRs (V001+) |

## Dependencies

| Prerequisite | Why | Notes |
|--------------|-----|--------|
| Mesh **nr → nu → n3** (reachability, UPnP/IPv6, circuit) | NAT’d / Client peers | Mobile is always Client (no listen) |
| Mesh **n4-media** blind `media_relay` | Group (a4) + NAT media | Org `pp-node` + desktop default on (N018); ↑/↓ + quote (N019 / V022); no payload decode |
| Peer `message_relay` | Offline inbox decentralization | **Separate** track — not a4 gate; HTTP Brief remains |
| Direct E2E + group messaging | Signaling + key wrap to invitees | Guests use direct pairwise only |
| Push Wave 1 + **`call_wake`** | Background ring | Extends push-notifications |
| Platform HW H264 (V017) + unified SDP (V019) | a3+ video encode/decode | Reuse in a4; no new device codec matrix |

## Progress snapshot

| Phase | Name | Status |
|-------|------|--------|
| v0 | Project docs + ADRs | Done |
| a0 | Mesh/SFU prerequisites alignment | **Done** |
| a1 | Signaling + session + history + ring wake | **Done** |
| a2 | 1:1 voice (WebRTC + LAN dogfood) | **Done** — LAN Opus OK; NAT unclaimed |
| a3 | 1:1 video (LAN; H264 platform HW; unified in-call) | **Done** — Android↔Win bidirectional; Linux receive-only (no camera); NAT unclaimed |
| a4 | Group ≤8 via **blind `media_relay`**, guests, rotate | Pending — V020–V022; soft-migrate; ↑/↓ quotes; pick rank TBD |
| a5 | Cap decision 8→16, polish | Pending |
| a6 | Promote contracts to `docs/` | Pending |
