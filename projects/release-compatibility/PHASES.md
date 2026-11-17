# Release compatibility: phases

Order of work. Status is in [CURRENT_STATE.md](CURRENT_STATE.md).

## P0 — Inventory

- [ ] Every format in [DESIGN § Scope](DESIGN.md#scope), per repo (pp-browser, pp-cpp-amp, pp-cpp-mesh, pp-cpp-common,
  pp-cpp-crypto, pp-relay-apis): its version, where the receiver checks it, what an unknown version or field does
  today.
- [ ] Each checked against [DESIGN § Release 1](DESIGN.md#release-1); the result is the list of release-1 work items,
  needed and worth-doing marked apart.
- [ ] The breaking work in flight that should land before the floor (open decision 5).
- [ ] Open decisions 1–6 settled, or left open with a default.

## P1 — Freeze

- [ ] No new breaking work outside the P0 list until release 1 ships.

## P2 — Release 1 built

- [ ] Each P0 item, repo by repo; the relay first (qa), as for every relay change.
- [ ] Frozen data of each release-1 format checked in, read by unit tests in each repo.

## P3 — Release 1 shipped

- [ ] Relay on qa, then the app lab; app, pp-node and relay to prod together.
- [ ] `min_client_version` and the node floor raised to release 1.
- [ ] The version table promoted to `docs/contracts/`; COMPATIBILITY.md's § No bridges replaced by the rule, its
  "Clean cutovers" table closed with the release-1 row; RC001–RC004 marked superseded by those docs.

## P4 — Release mode in force

- [ ] The PR template's *Contracts and docs* asks for the format, class, versions, range exit and rollback.
- [ ] CI: a signed-bytes change without a version bump fails, in every repo with signed objects.
- [ ] The mixed-version lab run (oldest and newest supported app, pp-node, relay) on released artifacts.
- [ ] The first format change under the range rule, as the worked example.
