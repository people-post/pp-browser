# Node operators — one operator for relays, pp-nodes and ledger relays

**Status:** Proposed ([N034](DECISIONS.md#n034--one-operator-for-relays-pp-nodes-and-ledger-relays)); after release 1, with
[N031](DECISIONS.md#n031--node-liveness-short-lived-signed-records-observed-by-the-mesh)'s node records
([NODE_LIVENESS.md](NODE_LIVENESS.md)). No code.  
**Date:** 2026-11-22  
**Related:** [release-compatibility SIGNED_FORMATS § 3](../release-compatibility/SIGNED_FORMATS.md#3-the-relay-operator-set-and-certificate)
(the relay operator set and certificate, RC018 item 7), [N032](DECISIONS.md#n032--node-versions-claims-only-exclude-verification-promotes)
rule 7 (count by operator), pp-ledger's [BEACON_AUTHORITY](https://github.com/people-post/pp-ledger/blob/main/docs/contracts/BEACON_AUTHORITY.md)
and [OPERATION_FILE](https://github.com/people-post/pp-ledger/blob/main/docs/contracts/OPERATION_FILE.md), pp-ledger-apps'
[fee app](https://github.com/people-post/pp-ledger-apps/blob/main/projects/fee-app/DESIGN.md) (FD004: relays' measures from
outside the chain)

## Problem

Three kinds of host are run by operators, and each names its operator differently, or not at all:

| Host | Who the operator is | What binds a host to them | Tool |
|------|---------------------|---------------------------|------|
| **The org relay** (pp-relay-apis) | an **operator set** (keys, a threshold, offline); its hash is the **relay id** clients pin | the **relay certificate**: transport key (PeerId), signing key, 1–4 origins, a rising sequence | `relay-operator.mjs` |
| **pp-node** | nobody: each node registers under **its own account** (`PP_NODE_IDENTITY_SEED` → device and account keys) | the node's own keys; N031's node record is signed by the node's account key | config and environment |
| **A ledger relay** (pp-node `--ledger-role relay`, or pp-ledger's `pp-relay`) | nobody: it runs on the node's identity, or on its own `keys/amp-identity.txt` | nothing; the chain's trust is the beacon certificate, and a relay signs no blocks | pp-ledger's `config.json` |

What that costs:

- **N032 rule 7 counts by `account_id`**, "at most one relay per account in a set". One person's ten nodes are ten
  accounts, so the rule groups nothing that a second node does not escape.
- **The fee app keeps a recipients list by hand** (FD004: name, PeerId, payout account). A host move or a new key is an
  edit to the holders' list, and the list is the holders' word, not the operator's.
- **Operators learn three procedures.** The org relay's operators already hold ML-DSA keys offline and sign with a
  threshold; pp-ledger-apps' key holders do the same for the chain. Node operators hold nothing.

## Direction

One **operator** (an operator set) stands behind any number of hosts. It signs, offline, a **certificate** per host
that binds the host's hot keys to it; the host serves the certificate beside what it already serves. The operator set
and the certificate's envelope are SIGNED_FORMATS § 3's, frozen for release 1; nodes add a second certificate type,
not a second operator format.

```
operator set (offline, t of n)  ──hash──►  operator id   (for the org relay: its relay id)
      │ signs
      ├── relay certificate  (unchanged)   transport key, signing key, origins           ← clients pin it
      └── node certificate   (new)         transport key, signing key, roles, payout     ← accountability
                                             │
                                  pp-node: its mesh roles and its ledger relay role, one certificate
```

### 1. The operator set is the relay's

`OperatorSet = u8 threshold ‖ u8 n ‖ n × LenBytes(typed signature key)`, 1 ≤ t ≤ n ≤ 8, keys sorted by bytes
(SIGNED_FORMATS § 3.1). Its hash is the **operator id**; for the org relay that is the relay id it already has, so
nothing about the org relay changes. An operator-set change (a lost or leaked key, § 3.6) works the same for any
operator. A volunteer node operator is usually 1 of 1; the org's sets stay as RC018 has them (prod and staging 2 of 3).

The operator id's written form needs a prefix of its own (`operator:…`), or the relay id's (`relay:…`) is reused for
every operator. Same hash either way; see [Open questions](#open-questions).

### 2. The node certificate

The relay certificate's envelope (`version ‖ LenBytes body ‖ signatures by index into the set in force`), under its own
signing domain, so a node certificate can never be read as a relay certificate:

```
signed  = "pp-browser:node-certificate" 00 ‖ u8 1 ‖ body
body    = LenBytes OperatorSet_founding                 → the operator id
        ‖ u64 k ‖ k × LenBytes(operator-set change)     as the relay certificate
        ‖ u64 sequence                                  ≥ 1, rising, per host
        ‖ i64 issued_at ‖ i64 expires_at                expires_at required (not 0): see § 4
        ‖ LenBytes transport key (typed)                the host's PeerId
        ‖ LenBytes signing key (typed) or empty         pp-node: its account key (it signs node records);
                                                        empty for a host that signs nothing (pp-ledger's pp-relay)
        ‖ u64 r ‖ r × LenUtf8 role                      what the operator stands behind: circuit_relay,
                                                        media_relay, directory, ledger_relay, ledger_miner
        ‖ ext area                                      payout account (§ 5), later fields
```

No origins and no addresses: nodes advertise their addresses as they change (registration, the node record, channel 0),
and pp-ledger keeps the beacon's address an unsigned dial hint for the same reason. The certificate binds **keys**.

A host is the pair (operator id, transport key). A sequence rises per host: a new key for the same machine is the next
sequence; a new machine is a new host under the same operator.

### 3. Where it travels, and who reads it

| Carrier | What it carries | Reader |
|---------|-----------------|--------|
| N031's **node record** (L1) | `operator_cert` (the certificate, or its hash with the certificate served once per link) | clients, directory nodes, the HTTP directory |
| `mesh_node` registration and `GET /v1/mesh/nodes` | the certificate, verbatim | clients |
| Channel 0 `CapabilityPayload` | the certificate's hash | a client that holds it already |
| pp-ledger `STATUS` meta (beside `certificate`, `signedTip`) | `operatorCertificate` on a ledger relay | the fee app, explorers |

Readers verify it as a relay certificate is verified (§ 3.2 of SIGNED_FORMATS: founding set, set changes, threshold,
no index twice, sequence kept per host and never lowered) and also that the **transport key is the PeerId the handshake
proved** — a certificate copied from another host proves nothing.

### 4. What it means: accountability, never trust

- **The org relay's certificate stays the only certificate clients pin.** It decides where messages go; a node
  certificate decides nothing about trust.
- **A node certificate groups and attributes.** N032 rule 7 counts by **operator id** where one is certified, by
  `account_id` otherwise; demotion for a contradicted claim (rule 6) reaches the operator. Operators are free to make,
  so a certificate is no reason to prefer a node (rule 4: claims only exclude), and it does not stop fake identities:
  rule 7's anchor and the cap on unknown nodes still do that. What it adds is that one real operator's many nodes count
  as one, and a demotion follows the operator to its other nodes.
- **A ledger relay certificate carries no ledger trust.** The chain trusts the beacon certificate and its proofs; a
  ledger relay's certificate only says who runs it, for fees and for explorers. pp-ledger's rules do not change.
- **It expires.** Nodes are many and less guarded than the org relay, so a stolen hot key is bounded by `expires_at`
  (default 90 days, the `mesh_node` registration's TTL), not only by spreading a higher certificate. Renewal is a
  re-sign of the same body with a later `expires_at` and the next sequence.
- **It is optional.** An uncertified node works as today, counted by its own account. A certified node is grouped with
  its operator's other nodes, and can be paid by certificate (§ 5).

### 5. Payouts

The node certificate may carry, in its extension area, the **payout account**: a pp-ledger account (or domain) the
operator names. The fee app's recipients list becomes a list of **operator ids**: it reads each relay's certificate
from the relay (`STATUS`) or the directory, takes the payout account from it, and adds up measures per operator across
its hosts. A host move needs no edit, and the payout account is the operator's own signed word, not a line the holders
typed. The measures stay the holders' (FD004); only who is paid moves into the certificate.

### 6. One tool

pp-ledger-apps' Signer already reviews and signs operations offline, from a locked vault. It learns three more kinds,
carried in the operation file's shape (OPERATION_FILE.md: `kind`, what is signed, `signers` with `minSignatures` and
`publicKeys`, `signatures` collected in any order, copies merged):

| Kind | Made by | Review shows |
|------|---------|--------------|
| `relay-certificate` | an online app, from what the relay serves (`/v1/relay-identity`) | what changes against the certificate in force: sequence, transport PeerId, origins, expiry |
| `node-certificate` | `pp-node operator request` prints it: the node's PeerId, account key and roles | the host, its roles, the payout account, the expiry; the last sequence signed for this host |
| `operator-set-change` | the online app | the keys leaving and joining, the threshold |

pp-node gains `operator request` (print the request), `operator install <file>` (take the signed certificate, checked
against its own keys) and a `/metrics` gauge for the certificate's expiry; pp-ledger's `pp-relay` the same two commands.
`relay-operator.mjs` keeps working for the org relay; the app writes the same bytes, against the same frozen vectors.

The relay certificate's C++ codec moves from pp-browser (`capabilities/net/src/lib/net/RelayCertificate.*`,
`capabilities/base/src/foundation/crypto/RelayId.*`) to a library pp-ledger-apps and pp-ledger can link (pp-cpp-common
beside `ExtArea`, or pp-cpp-crypto), after release 1.

## Phases

| Phase | What | Where |
|-------|------|-------|
| **O1 — Format** | The node certificate (§ 2) and its frozen vectors; the operator id's written form; the codec moved to a shared library with the relay certificate's | pp-cpp-common or pp-cpp-crypto, pp-browser docs (`docs/contracts/NODE_CERTIFICATE.md`) |
| **O2 — pp-node** | `operator request` / `install`; the certificate in registration and in N031's node record (L1); the expiry gauge | pp-browser (pp-node), pp-cpp-mesh (`NodeRecord`) |
| **O3 — Signer** | The three kinds in pp-ledger-apps' Signer and a relay-operator app (§ 6) | pp-ledger-apps |
| **O4 — Clients** | Verify; N032 rule 7 by operator id; demotion reaches the operator | pp-cpp-mesh, pp-browser |
| **O5 — Ledger relays** | `operatorCertificate` in `STATUS`; `pp-relay operator request` / `install`; the fee app's recipients by operator id, payout from the certificate | pp-ledger, pp-ledger-apps |

O1 lands before N031's L1 freezes the node record, so the record has its field from its first version.

## Release 1

None of this is in release 1. The relay certificate and the operator set are frozen as SIGNED_FORMATS has them, and
this design reuses them unchanged; the node certificate is a new signed object under its own domain (additive), and
N031, which it rides on, is after release 1 (RC017). Until then: no other operator key or set format anywhere.

## Open questions

- **The operator id's written form:** `operator:…` for every operator, the org relay's id staying `relay:…` (one hash, two
  prefixes by use), or `relay:…` for all. Default: `operator:` for node operators; a relay's id stays `relay:`.
- **Certificates required?** Default: optional for every node; org seeds certified by the org's operator set.
- **The payout account in the certificate?** Default: yes, as a signed extension; absent, the fee app's list names it.
- **Expiry and renewal cadence** for node certificates. Default: 90 days, renewed from 14 days before; pp-node warns from
  then on (log and gauge).
- **Who signs org seeds' certificates:** the org relay's operator set (one ceremony, a higher-value key used more often)
  or a separate org node operator set. Default: separate, 2 of 3, Leo's custody.
- **A pp-ledger account as the operator** (its keys and `minSignatures`, rotated by `account-update` on chain, the
  recovery set as the escape) instead of a founding set's hash. Set changes become ledger operations; the price is that
  checking a certificate needs ledger proofs. Not proposed now: the relay id is frozen as a founding set's hash.

## Not in scope

Anything that changes what clients trust for messaging (the org relay's certificate and pins, unchanged); pricing and
paid relays (DESIGN.md); the measures themselves (the fee app's FD004).
