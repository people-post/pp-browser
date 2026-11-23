# Archived projects

Projects whose delivery ended, with nothing open. They are kept, not deleted: code comments, docs and other
repositories cite their decision ids, and those ids stay valid here (A007 is still at-rest-crypto's A007, T001 still
thread-ownership's). What holds now is in [`docs/`](../../docs/README.md); a project's DESIGN and DECISIONS keep the
history and the reasons ([CR025](../capability-repos/DECISIONS.md#cr025--documentation-lives-with-what-it-describes-pp-specs-public-holds-shared-contracts-vectors-and-cross-repo-programs)).

A project moves here with `git mv` once its CURRENT_STATE says it is done and nothing is left open; every link to it
in this repository is fixed in the same change (no stubs). Do not add work to an archived project: start a new one,
or reopen it by moving it back.

| Project | Ids | What it delivered | Stable refs |
|---------|-----|-------------------|-------------|
| [at-rest-crypto](at-rest-crypto/) | A001–A015 | PIN-wrapped data key, encrypted `identity.enc` and stores, atomic file writes, Change PIN | [AT_REST_ENCRYPTION](../../docs/contracts/AT_REST_ENCRYPTION.md), [DATA_LAYOUT](../../docs/contracts/DATA_LAYOUT.md) |
| [libp2p-pq-transport](libp2p-pq-transport/) | P001–P006 | Post-quantum device identity (ML-DSA-65) and Noise (ML-KEM-768) in the libp2p fork, since retired for Amp | [AT_REST_ENCRYPTION](../../docs/contracts/AT_REST_ENCRYPTION.md), [LIBP2P_UPSTREAM](../../docs/architecture/LIBP2P_UPSTREAM.md) |
| [liquid-glass](liquid-glass/) | LG001–LG008 | Floating Chrome: opaque compact chrome, at most one frosted surface | [UI_DESIGN_SYSTEM § Floating Chrome](../../docs/ui/UI_DESIGN_SYSTEM.md#compact-floating-chrome-materials), [WINDOW_SHELL](../../docs/ui/WINDOW_SHELL.md) |
| [node-monitoring](node-monitoring/) | D001; M1–M3 | pp-node's `/metrics` and the app's own traffic and call-quality figures | [NODE_METRICS](../../docs/contracts/NODE_METRICS.md), [WINDOW_SHELL](../../docs/ui/WINDOW_SHELL.md) |
| [thread-ownership](thread-ownership/) | T001–T004; t1–t4 | Owner threads: calls and broadcast on the media-sessions owner, mesh control on its I/O lane | [THREADING § Owner threads](../../docs/architecture/THREADING.md#owner-threads) |
