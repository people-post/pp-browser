pp_browser_add_foundation_library(pp_foundation_crypto
  SOURCES
    AttachmentContentHash.cpp
    AttachmentContentCipher.cpp
    AutoKeyEstablishment.cpp
    CanonicalAad.cpp
    CryptoUtil.cpp
    DataKeyVault.cpp
    EncryptedPayload.cpp
    FileCipher.cpp
    HybridKem.cpp
    IdentitySeedDeriver.cpp
    MlDsa.cpp
    MessageCipher.cpp
    PinKeyDeriver.cpp
    PinResolver.cpp
    ProfileSecretsEngine.cpp
    ProfileUnlockGate.cpp
    PskBundleCodec.cpp
    LinkDeviceCodec.cpp
    PskFingerprint.cpp
    ReplayWindow.cpp
    SessionKeyDeriver.cpp
  PUBLIC_LIBS
    pp_foundation_error
    pp_foundation_data
    pp_crypto
    sodium)

if(PP_BROWSER_BUILD_TESTS)
  add_subdirectory(tests)
endif()

# Local dev builds only: bake a profile PIN in so the unlock prompt is skipped on every launch
# (also for iOS home-screen launches, which carry no environment). Never set in CI/release.
set(PP_BROWSER_DEV_PIN "" CACHE STRING "Dev only: profile PIN compiled into PinResolver fallback")
if(PP_BROWSER_DEV_PIN)
  target_compile_definitions(pp_foundation_crypto PRIVATE "PP_BROWSER_DEV_PIN=\"${PP_BROWSER_DEV_PIN}\"")
endif()
