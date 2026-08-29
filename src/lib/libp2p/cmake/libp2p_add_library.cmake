function(libp2p_add_library target)
  add_library(${target}
      ${ARGN}
      )
  if(PACKAGE_MANAGER STREQUAL "vendored")
    target_link_libraries(${target}
      qtils::qtils
      Asio::asio
      soralog::soralog
    )
  endif()
  libp2p_install(${target})
endfunction()
