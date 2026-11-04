include(PpBrowserBase)

# The product's base (CR015, CR023 step 3): its identity, platform hooks, text catalogs, config and data paths — the
# apps'. The shared base (runtime, TLS, errors, diagnostics, crypto, identity) is capabilities/base.
add_subdirectory(product)
add_subdirectory(platform)
add_subdirectory(i18n)
add_subdirectory(data)
