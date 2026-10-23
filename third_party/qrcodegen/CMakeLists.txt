# QR Code generator (Project Nayuki, C++) — upstream files imported by scripts/vendor/vendor_import.sh
add_library(qrcodegen STATIC qrcodegen.cpp)
target_include_directories(qrcodegen PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(qrcodegen PROPERTIES POSITION_INDEPENDENT_CODE ON)
