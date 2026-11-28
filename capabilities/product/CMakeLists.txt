# The product capability (projects/capability-repos CR028): the headless part of the product's base that both apps
# use identically — the build's identity (version, commit, dev tools), the platform core (backends and their compiled
# origins and relay certificates, the platform, its paths, the log defaults, a few OS helpers) and the data layout
# (paths, profiles, the mesh role, the config file's shared sections). Only apps include it (CR015); it builds on the
# base alone. The desktop's own config (AppConfig), its text catalogs and the GUI platform stay in src/foundation.
# Its src/ mirrors the tree ("foundation/…"); it goes to pp-cpp-core (CR027).
pp_browser_capability(product)

add_subdirectory(src/foundation/product)
add_subdirectory(src/foundation/platform)
add_subdirectory(src/foundation/data)
