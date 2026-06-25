function(libp2p_add_library target)
  add_library(${target}
      ${ARGN}
      )
  if(PACKAGE_MANAGER STREQUAL "vendored")
    target_link_libraries(${target}
      qtils::qtils
      Boost::boost
      soralog::soralog
      Boost::Boost.DI
    )
  endif()
  libp2p_install(${target})
endfunction()
