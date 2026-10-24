# Social posts — current state

**2026-10-23:** design draft (DESIGN.md, S001–S016).

**P1 landed (2026-10-23):** `lib/posts` (`pp_lib_posts`) — entries and head statements (canonical bytes,
BLAKE2b-256 hash without the signature, ML-DSA-65 sign / verify against the account id), every v1 kind's
body with the protocol limits, chain and fork checks, the profile's post store (forks kept side by side,
stubs for deleted and unkept entries, newest head per author, the device's own lane), and the device's
writer (`PostAuthor`: device-bound lane, never reuses a seq, claimed time monotonic per lane). 18 unit tests;
no UI, no network.

**Distribution designed (2026-10-23):** [DISTRIBUTION.md](DISTRIBUTION.md), S017–S021 — targets in the head
statement, the relay post API, `/pp/posts/1.0.0`, head discovery, publish outbox, refresh.

**Next:** review; then P2 — the relay PR first (merged and deployed before the client), then the client.
