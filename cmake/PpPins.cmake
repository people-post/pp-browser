# pp-browser's pin list: every people-post library this tree builds against, at one release tag each, in one place
# (projects/capability-repos CR012; DESIGN rule 4: the app owns the version set). Both apps built here read it:
# pp-browser and pp-node (the column says which needs which). A repo with its own apps (a capabilities repo,
# pp-ledger-apps) keeps a list of its own beside a copy of cmake/PpFetchPin.cmake.
#
# Bump a pin here, and only here. -D<PREFIX>_GIT_TAG=… overrides one tag for a build, -D<PREFIX>_SOURCE_DIR=… one
# library's source, -DPP_SIBLINGS_DIR=.. all of them (docs/ops/BUILD.md § Pins and sibling builds). The cmake/Pp<Lib>.cmake
# files add each library with its options; they hold no tags. During release 1's freeze the pins stay as they are
# (projects/release-compatibility PHASES P1).

include_guard(GLOBAL)
include(PpFetchPin)

#      repo           tag                  used by
pp_pin(pp-cpp-common  TAG v1.5.2)          # both: the runtime, JSON, logging
pp_pin(pp-cpp-crypto  TAG v1.4.1)          # both: sodium, ML-KEM / ML-DSA, BoringSSL
pp_pin(pp-cpp-amp     TAG v6.1.0)          # both: the Amp links
pp_pin(pp-cpp-mesh    TAG v10.2.0)         # both: the mesh host and its services
pp_pin(pp-cpp-core    TAG v0.1.0)          # both: base, product, people, net, node (cmake/PpCore.cmake)
pp_pin(pp-cpp-ui      TAG v0.6.3)          # pp-browser (GUI configures): the UI engine, SDL3 for media
pp_pin(pp-ledger      TAG release/v1.8.1)  # pp-node: the ledger role (PP_BROWSER_WITH_LEDGER)
pp_pin(pp-specs       TAG v1.0.0)          # tests (both): the shared contracts' frozen vectors (cmake/PpSpecs.cmake)
