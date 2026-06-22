# Builds (or reuses) a host protoc for cross-compiling Android/iOS libp2p protobuf codegen.

function(pp_browser_ensure_host_protoc)
  if(NOT (ANDROID OR CMAKE_SYSTEM_NAME STREQUAL "iOS"))
    return()
  endif()

  set(_host_build "${CMAKE_SOURCE_DIR}/build-host-protoc")
  set(_host_protoc "${_host_build}/protoc")

  if(NOT EXISTS "${_host_protoc}")
    message(STATUS "pp-browser: building host protoc at ${_host_build} (one-time)...")
    file(MAKE_DIRECTORY "${_host_build}")
    execute_process(
      COMMAND ${CMAKE_COMMAND}
        -S "${CMAKE_SOURCE_DIR}/third_party/protobuf/cmake"
        -B "${_host_build}"
        -DCMAKE_BUILD_TYPE=Release
        -DCMAKE_POLICY_VERSION_MINIMUM=3.5
        -Dprotobuf_BUILD_TESTS=OFF
        -Dprotobuf_INSTALL=OFF
        -Dprotobuf_WITH_ZLIB=OFF
        -Dprotobuf_BUILD_PROTOC_BINARIES=ON
      RESULT_VARIABLE _configure_result
    )
    if(NOT _configure_result EQUAL 0)
      message(FATAL_ERROR "pp-browser: host protoc configure failed (${_configure_result})")
    endif()
    execute_process(
      COMMAND ${CMAKE_COMMAND} --build "${_host_build}" --target protoc -j
      RESULT_VARIABLE _build_result
    )
    if(NOT _build_result EQUAL 0)
      message(FATAL_ERROR "pp-browser: host protoc build failed (${_build_result})")
    endif()
  endif()

  if(NOT EXISTS "${_host_protoc}")
    message(FATAL_ERROR "pp-browser: host protoc not found at ${_host_protoc}")
  endif()

  set(protobuf_BUILD_PROTOC_BINARIES OFF CACHE BOOL "" FORCE)
  set(Protobuf_PROTOC_EXECUTABLE "${_host_protoc}" CACHE FILEPATH "Host protoc for codegen" FORCE)
  set(ENV{PROTOBUF_PROTOC_EXECUTABLE} "${_host_protoc}")
  if(NOT TARGET protobuf::protoc)
    add_executable(pp_browser_protoc_host IMPORTED GLOBAL)
    set_target_properties(pp_browser_protoc_host PROPERTIES
      IMPORTED_LOCATION "${_host_protoc}")
    add_executable(protobuf::protoc ALIAS pp_browser_protoc_host)
  endif()
  message(STATUS "pp-browser: using host protoc ${Protobuf_PROTOC_EXECUTABLE}")
endfunction()
