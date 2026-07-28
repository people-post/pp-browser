# P2P A/V calls

**Status:** **a2 done** (LAN voice); **a3 planned** — H264 **platform HW** + shell path locked (V016–V018)  
**Owner:** Hongwei + agents  
**Stable refs:** (promote after ship) wire / wake / media-key contracts  
**Related:** [p2p-mesh](../p2p-mesh/) (circuit + audio/video SFU caps), [group-chat](../group-chat/), [e2e-message-crypto](../e2e-message-crypto/), [push-notifications](../push-notifications/), [P2P_MESSAGING.md](../../docs/architecture/P2P_MESSAGING.md)

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
| Mesh **audio/video SFU** on org seeds (`pp-node`) | Mobile↔mobile default path | More SFU seeds post-release (ops); **not a3 exit** (V016) |
| Direct E2E + group messaging | Signaling + key wrap to invitees | Guests use direct pairwise only |
| Push Wave 1 + **`call_wake`** | Background ring | Extends push-notifications |
| Platform HW H264 (V017) | a3 video encode/decode | Win MF / macOS VT / Android MediaCodec; Linux VA-API best-effort |

## Progress snapshot

| Phase | Name | Status |
|-------|------|--------|
| v0 | Project docs + ADRs | Done |
| a0 | Mesh/SFU prerequisites alignment | **Done** |
| a1 | Signaling + session + history + ring wake | **Done** |
| a2 | 1:1 voice (WebRTC + LAN dogfood) | **Done** — LAN Opus OK; NAT unclaimed |
| a3 | 1:1 video (LAN; H264 platform HW; shell tiles) | **Planned** — V016–V018 locked; SFU/iOS deferred |
| a4 | Group ≤8, guests, rotate-on-leave | Pending |
| a5 | Cap decision 8→16, polish | Pending |
| a6 | Promote contracts to `docs/` | Pending |
