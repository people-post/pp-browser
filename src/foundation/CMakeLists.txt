include(PpBrowserBase)

# The product's base (CR015, CR023 step 3): the apps'. Its headless part both apps use (the build's identity, the
# platform core, the data layout) is capabilities/product (CR028); here is the desktop app's: its platform hooks, text
# catalogs and config. The shared base (runtime, TLS, errors, diagnostics, crypto, identity) is capabilities/base.
add_subdirectory(platform)
add_subdirectory(i18n)
add_subdirectory(data)
