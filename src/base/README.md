# `src/base`

**Transitional aggregate only.** Domain peers now live under `src/domain/`; foundation
bands under `src/foundation/`. This directory keeps the `pp_base` INTERFACE that
links foundation modules plus Amp L1–L3 aliases (`pp_base_adp`,
`pp_base_mesh_session`, `pp_base_mesh_channel`, `pp_base_mesh_link` from
FetchContent [pp-cpp-amp](https://github.com/people-post/pp-cpp-amp)).

Product mesh glue is `src/domain/mesh/` (`pp_domain_mesh`).

North Star: [`docs/architecture/SRC_LAYOUT.md`](../../docs/architecture/SRC_LAYOUT.md).
