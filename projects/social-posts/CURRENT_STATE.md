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

**P2 relay side (2026-10-23):** the post relay service
[nfsc-brief-web3/app-posts#1](https://github.com/nfsc-brief-web3/app-posts/pull/1) (verify / keep / serve,
tested against `lib/posts` vectors; merged 2026-10-24) and the gateway route
[app-static-api#201](https://github.com/nfsc-brief-reader/app-static-api/pull/201)
(`/api/relay/v1/posts/*` → app-posts). Deploy needs an `app-posts` ECR repository, the service, and
`BRF_WWW_POSTS_URI`.

**Design round (2026-10-30):** S022–S029 — reposts and quotes carry a copy beside them and honor deletes;
relays moderate what they hold (withheld stubs, scan before serving media, anonymous or signed reports);
the final v1 post body (alt text, sizes, placeholder, facets, content warning and labels, languages,
`reply_root`) and an extension section on every entry; `post` stays short (long form a later `article`);
nothing is sent to another author's relays, notices come from follows.

**P1b landed (2026-10-30):** the S026 post body (media alt / size / duration / placeholder, facets with
UTF-8 boundary and order checks, `reply` with root and parent, content warning, labels, languages) and the
S027 extension section on every entry, with their limits; report sign bytes (S025). Vectors regenerated:
a full post, a reply that quotes, an entry with extensions, and two reports. 21 unit tests.

**Relay for P2 (2026-10-30):** the new format in the relay
([app-posts#2](https://github.com/nfsc-brief-web3/app-posts/pull/2), merged); withheld stubs, suspended authors,
the scan step before media is served, reports, refusal codes, the single-entry read and the operator's
`moderate` CLI ([app-posts#3](https://github.com/nfsc-brief-web3/app-posts/pull/3), in review); the gateway
serving registered accounts only, refusing other authors' writes as `not_served`
([app-static-api#217](https://github.com/nfsc-brief-reader/app-static-api/pull/217), in review).

**Next:**
1. Merge and deploy the relay (app-posts ECR repository and service, `BRF_WWW_POSTS_URI`).
2. The client side of P2.
