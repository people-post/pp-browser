# The base (projects/capability-repos CR023 step 3): the mechanisms every capability uses — common (the shared
# vocabulary, text by key, metrics and diagnostics lines, the runtime bridge) and foundation's runtime, TLS, errors,
# diagnostics, crypto and identity. On pp-cpp-* only. The product's own base (config, data paths, platform hooks,
# text catalogs, its identity) stays in src/foundation: the apps'. Its src/ mirrors the tree it came from
# ("common/…", "foundation/…"); it is an include root of the base's targets and reaches the rest by link.
pp_browser_capability(base)

add_subdirectory(src/common)
add_subdirectory(src/foundation)
