# calls — capability docs

Audio / video calls: the call vocabulary and its store ports, the call sounds (`call_audio`), the media legs on the mesh (`call_media`) and the session orchestration (`feature/calls`), on media's engine and devices. Delivery carries its signaling; it never uses messaging. It is the first capability made repo-ready (CR023): it builds and tests on its own, and its host contract — the ports an app fills — is in [the folder's README](../README.md), with how to build it standalone.

**Paths** (under this folder; includes read as before, e.g. `"lib/calls/…"`): [`src/lib/calls/`](../src/lib/calls/), [`src/lib/call_audio/`](../src/lib/call_audio/), [`src/common/media/`](../src/common/media/), [`src/domain/call_media/`](../src/domain/call_media/), [`src/feature/calls/`](../src/feature/calls/). Its targets are in [`CMakeLists.txt`](../CMakeLists.txt); its tests carry the CTest label `capability:calls`.

**Below it:** people, net, delivery, pricing, media and the base ([capability-repos DESIGN § Target](../../../projects/capability-repos/DESIGN.md#target), [SRC_LAYOUT § Capabilities (across layers)](../../../docs/architecture/SRC_LAYOUT.md#capabilities-across-layers)).

## Docs about it

- Architecture: [CALLS](../../../docs/architecture/CALLS.md)
- Contracts: [MEDIA_CHANNELS](../../../docs/contracts/MEDIA_CHANNELS.md), [WIRE_SCHEMAS](../../../docs/contracts/WIRE_SCHEMAS.md) (§ Media frames), [AMP-CHANNEL § Call-media bundle](../../../docs/contracts/AMP-CHANNEL.md#call-media-bundle) (a call's channels: path migration, failover, `bye`); the transport under them: [pp-cpp-amp's docs](https://github.com/people-post/pp-cpp-amp/blob/main/README.md#docs)
- Projects: [p2p-av-calls](../../../projects/p2p-av-calls/), [call-path-resilience](../../../projects/call-path-resilience/), [media-client-layers](../../../projects/media-client-layers/), [adp](../../../projects/adp/) ([CALL_MEDIA_CIRCUIT](../../../projects/adp/CALL_MEDIA_CIRCUIT.md): a call through a circuit)
- Decisions: [CR003](../../../projects/capability-repos/DECISIONS.md#cr003--calls-and-broadcast-are-one-repo-realtime), [CR020](../../../projects/capability-repos/DECISIONS.md#cr020--secure-delivery-below-messaging-and-realtime), [CR023](../../../projects/capability-repos/DECISIONS.md#cr023--calls-becomes-repo-ready-first), [CR024](../../../projects/capability-repos/DECISIONS.md#cr024--capabilities-own-ideas-the-app-owns-the-database)

This folder holds the capability's own docs and leaves with its code when the capability leaves pp-browser ([CR025](../../../projects/capability-repos/DECISIONS.md#cr025--documentation-lives-with-what-it-describes-pp-specs-public-holds-shared-contracts-vectors-and-cross-repo-programs), [CR005](../../../projects/capability-repos/DECISIONS.md#cr005--move-a-capability-only-when-a-second-app-needs-it)). The contracts and projects above stay where they are until then; shared ones go to pp-specs after release 1.
