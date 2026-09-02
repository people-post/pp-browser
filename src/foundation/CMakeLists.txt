include(PpBrowserBase)

# Foundation bands (ordered): runtime → error/i18n; platform/data/crypto follow in later peels.
add_subdirectory(runtime)
add_subdirectory(i18n)
add_subdirectory(error)
