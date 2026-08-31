function(libp2p_add_library target)
  add_library(${target}
      ${ARGN}
      )
  if(PACKAGE_MANAGER STREQUAL "vendored")
    # A017 PeerId/wire: qtils + soralog only (no Asio Host underlay).
    target_link_libraries(${target}
      qtils::qtils
      soralog::soralog
    )
  endif()
  libp2p_install(${target})
endfunction()
