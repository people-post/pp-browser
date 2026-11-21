# Release compatibility: the signed-format cutover, work plan

How [SIGNED_FORMATS.md](SIGNED_FORMATS.md) ([RC020](DECISIONS.md#rc020--the-signed-formats-for-the-release-1-cutover)) is built and landed. The formats
are in SIGNED_FORMATS; this page is who builds what, where it lands, and in which order.

## Branches

The old and new forms cannot read each other, so nothing lands on `main` piece by piece (main deploys the relay to qa,
RC019). Each repo gets one integration branch; every work item is a PR into it, reviewed and merged there as it is
ready; merge day merges each integration branch into `main`.

| Repo | Integration branch | Lands on main |
|------|--------------------|---------------|
| pp-browser | `cutover/signed-formats` | merge day, with the relay's |
| pp-relay-apis | `cutover/signed-formats` | merge day (deploys qa) |
| pp-cpp-mesh | none: DHT record v2 is a library release (`v10.0.0`, a breaking format), pinned on pp-browser's branch | before merge day |

The integration branches are rebased on (or merge) `main` weekly, so they do not drift. CI runs on PRs into them.

## Work items

| Id | Repo | What (SIGNED_FORMATS §) | Owns (files) | Needs |
|----|------|--------------------------|--------------|-------|
| W0 | pp-browser | The shared pieces: typed keys and signatures over `AlgId`, `ExtArea` in signed bytes, one helper for the receiver's order (version → parse → algorithm ids → verify → extensions, § 1.1), the vector-file harness, `docs/contracts/SIGNED_EXTENSIONS.md` (§ 1's registry); copies of pp-cpp-common's and pp-cpp-crypto's vector files | `capabilities/base/src/foundation/crypto/` (new files), test harness, the new contract | the library tags (done) |
| W1 | pp-browser | Envelope v4 (§ 2.1), `PeerRequestAuth` v2 (§ 2.7) | delivery `EnvelopeSigner`, `RelayEnvelope`, `MessagingJson` envelope parts, `PeerRequestAuth`; WIRE_SCHEMAS § envelope, MESSAGE_ENCRYPTION | W0 |
| W2 | pp-browser | Account card v5 and key card (§ 2.2), KEM binding v2 (§ 2.3) | people `AccountCard`, `KemKeyBinding`, card codecs; ACCOUNT_CARD | W0, W5's relay id type |
| W3 | pp-browser | Relay grant v2 (§ 2.4), roster statement / dissolved v4, owner transfer v2, roster change v2 (§ 2.5), locker record v2 and state (§ 2.6, with T7) | `RelayGrantSigner`, `GroupRosterStatement`, `GroupChange`, `AccountLocker`, `AccountStateCodec`; ACCOUNT_LOCKER | W0 |
| W4 | pp-browser + pp-cpp-mesh | Broadcast join ticket and peer-announce tip v2 (§ 2.9); DHT record v2 (§ 2.10) in pp-cpp-mesh, tagged `v10.0.0` | broadcast `BroadcastJoinTicket`, `PeerAnnounceCodec`; mesh `DhtRecordCodec` | W0 (mesh: the library tags) |
| W5 | pp-browser | Relay id, operator set, relay certificate (§ 3): `RelayCertificate` (parse, verify, keep the highest), `RelayPins` by relay id, `RelayIdentity` v2, `client-compat`'s certificate, `RelayAudience` from it, `ParseRelayLink` (`#relay:…`), locators, `DeploymentProfile` and build scripts (each environment's relay id and certificate), pp-node `registration.relay_id`; `relay_certificate_vectors.json` | `lib/net` relay identity/pins/audience/link, `ContactLocator` relay entry, `DeploymentProfile`, node registration; SERVICE_ENDPOINTS (identity, pins, audience), MESH_IDENTITY (relay link) | W0 |
| W6 | pp-browser | Relay signed calls v4, every domain (§ 2.8): `RelaySignBytes` and every relay sign payload, typed signatures, `ext_b64`, no `signature_alg`; `relay_sign_vectors.json` v2 (F5) | `RelaySignBytes`, `Relay*SignPayload`, `RegistrationSignPayload`, the relay clients' body builders | W0, W2 (card in register), W5 (audience) |
| W7 | pp-browser | Leave the relay's legacy routes (INVENTORY C1, RC018 item 2): `GET /v1/search` → by-account, `blobs/*` → `cdn/*`, `files/*` → `blob/*`, `keys/rotate` + `llm_api_key` → `llm_token` + `token/renew`, the relay's LLM codes, registered-or-guest by the key's slot | `HttpBlobClient`, `RelayFileUrl`, search, `ConversationsHub` key rotation, `lib/ai` limits, `AgentSessions` | none (lands on main before merge day: today's relay serves both) |
| R1 | pp-relay-apis | `ext-area.ts`, `alg-id.ts` (the libraries' vectors byte for byte); sign domains v4 (§ 2.8, § 4), `relay_upgrade_required`, the legacy sign window and `LEGACY` builders removed (C2, its sign half) | `packages/shared/src/sign-payload/`, `crypto/alg-id.ts`, `auth.ts` | W0's vector copies; W6's `relay_sign_vectors.json` v2 |
| R2 | pp-relay-apis | Card v5 stored verbatim, KEM binding v2, grant v2, locker record v2 (§ 4) | `account-card.ts`, `repos/accounts.ddb.ts`, registration, `grant.ts`, messenger send, `crypto/locker.ts`, lockers | R1, W2, W3 |
| R3 | pp-relay-apis | The certificate: the registry loads it (`RELAY_CERTIFICATE_SECRET_ID`, `RELAY_SIGNING_KEY_SECRET_ID`), checks it, serves `relay-identity` v2 and `client-compat`'s `relay_certificate_b64`; `scripts/relay-operator.mjs`; the local stack's 1-of-1 set | registry `relay-identity.ts`, `client-compat.ts`, shared `relay-identity.ts`, `scripts/`, `local-stack/` | R1, W5's vectors |
| R4 | pp-relay-apis | The legacy routes removed (C2, its route half: `blobs/*`, `files/*`, `GET /search`, `llm_api_key`, `keys/rotate`, the pre-cutover sender mapping) and the one-time cutover data job (Q12) | `storage/src/routes.ts`, registry routes, `llm-credentials.ts`, `records.ts`, a job | W7 on main |
| R5 | pp-relay-apis | Support (the support worker) speaks the release-1 app ↔ app formats: envelope v4, account card v5 (its own and customers'), KEM binding v2, relay grant v2; its own card back on register. Its signed relay calls move to v4 with R1. Missed by the first plan (INVENTORY lists Support on the app's formats) | `packages/support-crypto`, `functions/support-worker` ingest/outbound | R2b, W1, W2 vectors |
| L | both | The app lab against the local stack on the integration branches, every suite; then against qa after merge day | — | everything |

Infrastructure and keys, **Leo** (asked 2026-11-20; parallel to the work above): the certificate and signing-seed
secrets and the registry's permission to read them (terraform); a qa operator set (2 of 3, or 1 of 1 until staging
exists) and its certificate 1 before merge day; prod's set and certificate 1 before release day (§ 7 step 5);
pp-node's fleet config naming the relay id (ansible) for pp-node 10.0.

How they were split while built: R1 as **R1a** (`ext-area.ts`, `alg-id.ts`) and **R1b** (sign domains v4, after W6);
R2 as **R2a** (grant v2, locker v2) and **R2b** (card v5, KEM binding v2, after W2). PRs into an integration branch
run no CI (its items run the full suite locally); merged items land in batches, and the integration branch's push
builds each batch once.

### Infrastructure and keys, Leo

What the merged R3, R4 and W5 need, exactly (the relay's `docs/operators.md` has the steps and a worked example):

1. Per environment, two new secrets: `pp-relay-api/<env>/relay-signing-key` (standard base64 of a 32-byte seed) and
   `pp-relay-api/<env>/relay-certificate` (the certificate's standard base64, `relay-operator.mjs combine` output).
2. Registry variables `RELAY_SIGNING_KEY_SECRET_ID`, `RELAY_CERTIFICATE_SECRET_ID`, `RELAY_ID` (`relay:…`) and
   `RELAY_ORIGINS` (the API bases exactly as clients use them, `https://<host>/api/relay`); `RELAY_IDENTITY_PEER_ID`
   equals the certificate's audience. The support worker needs the same `RELAY_ID` (R5: it names its relay on
   Support's card, and stops at start without it). IAM: `secretsmanager:GetSecretValue` on both secrets for the registry role
   (and `kms:Decrypt` with a customer-managed key).
3. qa's operator set (2 of 3, or 1 of 1 until staging exists) and its certificate 1 (sequence 1, `expires_at` 0) before
   merge day: without it qa's registry answers `503 relay_identity_unavailable` to signed calls. Prod's before release
   day, certifying prod's current transport key and a new signing seed.
4. pp-browser: the relay ids in CI variables `PP_RELAY_ID_QA` / `PP_RELAY_ID_PROD`, the certificates in
   `config/relay-certs/{qa,prod}.cert` (until then builds fail closed against qa and prod); pp-node 10.0's fleet config
   sets `registration.relay_id` (ansible).
5. The cutover data job (`packages/cutover-data`, dry run unless `--apply`): a role to run it once per environment
   (qa on merge day, prod on release day) with DynamoDB scan/query/update/delete on `accounts` and `messages`, and S3
   delete on the locker and message-body prefixes. After merge day the CloudFront behaviours for the removed routes
   (`keys/rotate`, `/search`, `blobs/*`, `files/*`) and `LLM_LEGACY_KEYS_ACCEPTED_UNTIL` can go.

## Order

1. **W0**, then W1–W6 in parallel (W2 and W6 take W5's relay id type as soon as it is on the integration branch);
   **W7** to `main` meanwhile; **W4's mesh half** tagged `v10.0.0` and pinned on pp-browser's integration branch.
2. **R1**, then R2 and R3, each reading pp-browser's vector files byte for byte; **R4** once W7 is on `main`.
3. **L**: the whole app lab on the two integration branches against the local stack (1-of-1 operator set).
4. **Merge day**, when Leo's qa secrets and certificate exist: pp-relay-apis then pp-browser integration branches into
   `main` the same day; qa redeploys; testers update (old and new builds cannot talk). The lab runs against qa.
5. **Release day** as SIGNED_FORMATS § 7 step 6.

## Status

See [CURRENT_STATE.md](CURRENT_STATE.md).
