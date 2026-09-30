# Privacy — design

## Goal

A user can tell, and bound, what others learn about them from using pp-browser. Content is already end-to-end encrypted ([MESSAGE_ENCRYPTION](../../docs/contracts/MESSAGE_ENCRYPTION.md)) and at rest behind the PIN vault ([AT_REST_ENCRYPTION](../../docs/contracts/AT_REST_ENCRYPTION.md)); this project covers what is left: **addresses, metadata, presence, discoverability, and what we write down about users**.

## Parties and what they can learn

| Party | Today it can learn | Theme |
|-------|--------------------|-------|
| Any peer that knows our PeerId | Our public IP (punch target answers anyone; directory lookup), then LAN IPs over ch0 once linked | T1 |
| A peer we message or call | Our IP when the path is direct; our addresses in call signalling | T1 |
| Relay operators (circuit, media_relay) | Both endpoints' IPs; who talks to whom and when; traffic volume | T1, T4 |
| Org server (Brief relay, directory) | IP; registration addresses; message routing metadata (sender, recipient, timing, size) | T2 |
| The LAN | PeerId and LAN addresses from mDNS (Node role / mobile in-call) | T1 |
| Strangers generally | Whether we are online / live (announce tips, DHT, directory), whether a message was read | T3, T5 |
| Anyone with the device or its logs | Peer ids, addresses and names in logs / diagnostics | T6 |

## Themes

- **T1 — Network address disclosure.** Our IP goes only to peers the user allows; everyone else reaches us through a relay. The unit is *disclosure of our address*, not "a direct connection": a peer learns our IP when we dial it, punch toward it, answer its punch, send it our listen addresses (call signalling, ch0, directory), or it finds them published. Each of those is gated by one audience setting ([P001](DECISIONS.md#p001--the-unit-is-address-disclosure)).
- **T2 — Server-side metadata.** What the org server and Brief relay see (routing metadata, directory lookups, IP). Minimise, then document what remains.
- **T3 — Discoverability and inbound control.** Who can find, message, call or add us; Blocked enforced at the link, not only in the UI.
- **T4 — Relay trust.** Which relays may carry our traffic (they see both IPs); a strict mode limited to org seeds and trusted contacts' nodes.
- **T5 — Presence and activity.** Online / live / typing / read state, and who gets it.
- **T6 — Logs, diagnostics, metrics.** No peer ids, addresses or content outside debug-only paths; diagnostics copy redacts by default. (`/metrics` already follows the rule — [NODE_METRICS](../../docs/contracts/NODE_METRICS.md).)

## T1 shape — the direct audience

One setting, **Direct connections**: `everyone` | `contacts` (default) | `friendly` | `nobody` ([P002](DECISIONS.md#p002--default-audience-contacts)).

- **Allowed peer**: in the audience and not Blocked. `contacts` = any saved contact; `friendly` = contacts marked Friendly; `nobody` = no one. Blocked is never allowed, whatever the setting.
- **Gate** (`common/privacy/AddressDisclosure.h`): the feature layer (hub) classifies contacts into PeerId sets and publishes a snapshot; mesh and calls consult `AllowsDirect(peer_id)` from any thread. `domain/mesh` never reads contacts.
- **For a peer that is not allowed** we do not: dial it directly or punch toward it (calls, chat, LAN), answer its punch as target, send it our listen addresses in call signalling, or (P2) in ch0. We reach it through a circuit relay; chat falls back to the Brief relay.
- **Links that already exist are used** ([P003](DECISIONS.md#p003--existing-links-are-used)): the disclosure happened when the link came up.
- **pp-node is exempt** ([P004](DECISIONS.md#p004--nodes-are-public-by-role)): a Node's job is to be reachable.
- **What T1 cannot hide**: our IP from relay operators (T4) and the org server (T2), and from anyone who already has it.

## Not in scope

Anonymity against a global observer (onion routing, cover traffic). Content crypto (done elsewhere).
