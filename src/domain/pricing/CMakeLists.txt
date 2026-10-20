# Initiation billing and payment promises: codecs, stores, lifecycle, and their system-control
# families (projects/pricing/).
pp_browser_add_domain_library(pp_domain_pricing
  SOURCES
    InitiationBillingCodec.cpp
    InitiationBillingStore.cpp
    InitiationPricing.cpp
    PaymentPromiseAvoid.cpp
    PaymentPromiseCodec.cpp
    PaymentPromiseLifecycle.cpp
    PaymentPromiseStore.cpp
    PaymentPromiseWireCodec.cpp
    PricingControlFamilies.cpp
  PUBLIC_LIBS
    pp_foundation_error
    pp_foundation_data
    pp_foundation_crypto)

if(PP_BROWSER_BUILD_TESTS)
  add_subdirectory(tests)
endif()
