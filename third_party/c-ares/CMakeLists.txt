cmake_minimum_required (VERSION 3.1.0)

include (CheckIncludeFiles)
include (CheckTypeSize)
include (CheckFunctionExists)
include (CheckSymbolExists)
include (CheckCSourceCompiles)
include (CheckStructHasMember)
include (CheckLibraryExists)
include (GNUInstallDirs)

project (c-ares C)

# Set this version before release
set (CARES_VERSION "1.14.0")

# This is for libtool compatibility, and specified in a form that is easily
# translatable from libtool (even if the actual form doesn't make sense).
# For instance, in an autotools project, in Makefile.am there is a line that
# contains something like:
#     -version-info 4:0:2
# This breaks down into sections of current:revision:age
# This then generates a version of  "(current-age).age.revision"  with an
# interface version of "(current-age)"
# For example, a version of 4:0:2 would generate output such as:
#    libname.so   -> libname.so.2
#    libname.so.2 -> libname.so.2.2.0
set (CARES_LIB_VERSIONINFO "4:0:2")


option (CARES_STATIC     "Build as a static library"                                             OFF)
option (CARES_SHARED     "Build as a shared library"                                             ON)
option (CARES_INSTALL    "Create installation targets (chain builders may want to disable this)" ON)
option (CARES_STATIC_PIC "Build the static library as PIC (position independent)"                OFF)
option (CARES_BUILD_TESTS "Build and run tests"                                                  OFF)
option (CARES_BUILD_TOOLS "Build tools"                                                          ON)

# allow linking against the static runtime library in msvc
if (MSVC)
  option (CARES_MSVC_STATIC_RUNTIME "Link against the static runtime library" OFF)
  if (CARES_MSVC_STATIC_RUNTIME)
    # CMAKE_CONFIGURATION_TYPES is empty on non-IDE generators (Ninja, NMake)
    # and that's why we also use CMAKE_BUILD_TYPE to cover for those generators.
    # For IDE generators, CMAKE_BUILD_TYPE is usually empty
    foreach (config_type ${CMAKE_CONFIGURATION_TYPES} ${CMAKE_BUILD_TYPE})
      string (TOUPPER ${config_type} upper_config_type)
      set (flag_var "CMAKE_C_FLAGS_${upper_config_type}")
      if (${flag_var} MATCHES "/MD")
        string (REGEX REPLACE "/MD" "/MT" ${flag_var} "${${flag_var}}")
      endif ()
    endforeach ()

    # clean up
    set (upper_config_type)
    set (config_type)
    set (flag_var)
  endif ()
endif ()

# Keep build organized.
set (CMAKE_RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/bin)
set (CMAKE_LIBRARY_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/lib)
set (CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/lib)
set (PACKAGE_DIRECTORY ${PROJECT_BINARY_DIR}/package)

# Destinations for installing different kinds of targets (pass to install command).
set (TARGETS_INST_DEST
  RUNTIME DESTINATION  ${CMAKE_INSTALL_BINDIR}
  LIBRARY DESTINATION  ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION  ${CMAKE_INSTALL_LIBDIR}
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

# Look for dependent/required libraries
check_library_exists (resolv res_servicename "" HAVE_RES_SERVICENAME_IN_LIBRESOLV)
if (HAVE_RES_SERVICENAME_IN_LIBRESOLV)
  set (HAVE_LIBRESOLV 1)
endif ()

if (APPLE)
  check_c_source_compiles ("
  #include <stdio.h>
  #include <TargetConditionals.h>
  int main() {
#if TARGET_OS_IPHONE == 0
#error Not an iPhone target
#endif
return 0;
  }
  "
  IOS)

  check_c_source_compiles ("
#include <stdio.h>
#include <TargetConditionals.h>
  int main() {
#if TARGET_OS_IPHONE == 0 || __IPHONE_OS_VERSION_MIN_REQUIRED < 100000
#  error Not iOS v10
#endif
return 0;
  }
  "
  IOS_V10)

  check_c_source_compiles ("
#include <stdio.h>
#include <AvailabilityMacros.h>
#ifndef MAC_OS_X_VERSION_10_12
#  define MAC_OS_X_VERSION_10_12 101200
#endif
  int main() {
#if MAC_OS_X_VERSION_MIN_REQUIRED < MAC_OS_X_VERSION_10_12
#  error Not MacOSX 10.12 or higher
#endif
return 0;
  }
  "
  MACOS_V1012)
endif ()

if (IOS AND HAVE_LIBRESOLV)
  set (CARES_USE_LIBRESOLV 1)
endif()

check_library_exists (nsl gethostbyname "" HAVE_LIBNSL)
check_library_exists (socket gethostbyname "" HAVE_GHBN_LIBSOCKET)
check_library_exists (socket socket "" HAVE_SOCKET_LIBSOCKET)
if (HAVE_GHBN_LIBSOCKET OR HAVE_SOCKET_LIBSOCKET)
  set(HAVE_LIBSOCKET TRUE)
endif ()
check_library_exists (rt clock_gettime "" HAVE_LIBRT)


# Look for necessary includes
check_include_files (sys/types.h           HAVE_SYS_TYPES_H)
check_include_files (sys/socket.h          HAVE_SYS_SOCKET_H)
check_include_files (arpa/inet.h           HAVE_ARPA_INET_H)
check_include_files (arpa/nameser_compat.h HAVE_ARPA_NAMESER_COMPAT_H)
check_include_files (arpa/nameser.h        HAVE_ARPA_NAMESER_H)
check_include_files (assert.h              HAVE_ASSERT_H)
check_include_files (errno.h               HAVE_ERRNO_H)
check_include_files (fcntl.h               HAVE_FCNTL_H)
check_include_files (inttypes.h            HAVE_INTTYPES_H)
check_include_files (limits.h              HAVE_LIMITS_H)
check_include_files (malloc.h              HAVE_MALLOC_H)
check_include_files (memory.h              HAVE_MEMORY_H)
check_include_files (netdb.h               HAVE_NETDB_H)
check_include_files (netinet/in.h          HAVE_NETINET_IN_H)
check_include_files (netinet/tcp.h         HAVE_NETINET_TCP_H)
check_include_files (net/if.h              HAVE_NET_IF_H)
check_include_files (signal.h              HAVE_SIGNAL_H)
check_include_files (socket.h              HAVE_SOCKET_H)
check_include_files (stdbool.h             HAVE_STDBOOL_H)
check_include_files (stdint.h              HAVE_STDINT_H)
check_include_files (stdlib.h              HAVE_STDLIB_H)
check_include_files (strings.h             HAVE_STRINGS_H)
check_include_files (string.h              HAVE_STRING_H)
check_include_files (stropts.h             HAVE_STROPTS_H)
check_include_files (sys/ioctl.h           HAVE_SYS_IOCTL_H)
check_include_files (sys/param.h           HAVE_SYS_PARAM_H)
check_include_files (sys/select.h          HAVE_SYS_SELECT_H)
check_include_files (sys/socket.h          HAVE_SYS_SOCKET_H)
check_include_files (sys/stat.h            HAVE_SYS_STAT_H)
check_include_files (sys/time.h            HAVE_SYS_TIME_H)
check_include_files (sys/types.h           HAVE_SYS_TYPES_H)
check_include_files (sys/uio.h             HAVE_SYS_UIO_H)
check_include_files (time.h                HAVE_TIME_H)
check_include_files (dlfcn.h               HAVE_DLFCN_H)
check_include_files (unistd.h              HAVE_UNISTD_H)

# Include order matters for these windows files.
check_include_files ("winsock2.h;windows.h"            HAVE_WINSOCK2_H)
check_include_files ("winsock2.h;ws2tcpip.h;windows.h" HAVE_WS2TCPIP_H)
check_include_files ("winsock.h;windows.h"             HAVE_WINSOCK_H)
check_include_files (windows.h                         HAVE_WINDOWS_H)


# Set system-specific compiler flags
if (CMAKE_SYSTEM_NAME STREQUAL "Darwin")
  list (APPEND SYSFLAGS -D_DARWIN_C_SOURCE)
elseif (CMAKE_SYSTEM_NAME STREQUAL "Linux")
  list (APPEND SYSFLAGS -D_GNU_SOURCE -D_POSIX_C_SOURCE=199309L -D_XOPEN_SOURCE=600)
elseif (CMAKE_SYSTEM_NAME STREQUAL "SunOS")
  list (APPEND SYSFLAGS -D__EXTENSIONS__ -D_REENTRANT -D_XOPEN_SOURCE=600)
elseif (CMAKE_SYSTEM_NAME STREQUAL "AIX")
  list (APPEND SYSFLAGS -D_ALL_SOURCE -D_XOPEN_SOURCE=600)
elseif (CMAKE_SYSTEM_NAME STREQUAL "FreeBSD")
  # Don't define _XOPEN_SOURCE on FreeBSD, it actually reduces visibility instead of increasing it
elseif (WIN32)
  list (APPEND SYSFLAGS -DWIN32_LEAN_AND_MEAN -D_CRT_SECURE_NO_DEPRECATE -D_CRT_NONSTDC_NO_DEPRECATE -D_WIN32_WINNT=0x0600)
endif ()
add_definitions(${SYSFLAGS})



# Tell C-Ares about libraries to depend on
if (HAVE_LIBRESOLV)
  list (APPEND CARES_DEPENDENT_LIBS resolv)
endif ()
if (HAVE_LIBNSL)
  list (APPEND CARES_DEPENDENT_LIBS nsl)
endif ()
if (HAVE_LIBSOCKET)
  list (APPEND CARES_DEPENDENT_LIBS socket)
endif ()
if (HAVE_LIBRT)
  list (APPEND CARES_DEPENDENT_LIBS rt)
endif ()
if (WIN32)
  list (APPEND CARES_DEPENDENT_LIBS ws2_32)
endif ()


# When checking for symbols, we need to make sure we set the proper
# headers, libraries, and definitions for the detection to work properly
# CMAKE_REQUIRED_DEFINITIONS, CMAKE_REQUIRED_LIBRARIES, and
# CMAKE_EXTRA_INCLUDE_FILES.  When we're done with the detection, we'll
# unset them.

set (CMAKE_REQUIRED_DEFINITIONS ${SYSFLAGS})
list (APPEND CMAKE_REQUIRED_LIBRARIES ${CARES_DEPENDENT_LIBS})

macro (cares_extrainclude_ifset var include)
  if (${var})
    list (APPEND CMAKE_EXTRA_INCLUDE_FILES ${include})
  endif ()
endmacro ()

cares_extrainclude_ifset (HAVE_STDBOOL_H      stdbool.h)
cares_extrainclude_ifset (HAVE_SYS_TYPES_H    sys/types.h)
cares_extrainclude_ifset (HAVE_ARPA_INET_H    arpa/inet.h)
cares_extrainclude_ifset (HAVE_ARPA_NAMESER_H arpa/nameser.h)
cares_extrainclude_ifset (HAVE_NETDB_H        netdb.h)
cares_extrainclude_ifset (HAVE_NET_IF_H       net/if.h)
cares_extrainclude_ifset (HAVE_NETINET_IN_H   netinet/in.h)
cares_extrainclude_ifset (HAVE_NETINET_TCP_H  netinet/tcp.h)
cares_extrainclude_ifset (HAVE_SIGNAL_H       signal.h)
cares_extrainclude_ifset (HAVE_STDLIB_H       stdlib.h)
cares_extrainclude_ifset (HAVE_STRING_H       string.h)
cares_extrainclude_ifset (HAVE_STRINGS_H      strings.h)
cares_extrainclude_ifset (HAVE_SYS_IOCTL_H    sys/ioctl.h)
cares_extrainclude_ifset (HAVE_SYS_SELECT_H   sys/select.h)
cares_extrainclude_ifset (HAVE_SYS_SOCKET_H   sys/socket.h)
cares_extrainclude_ifset (HAVE_SYS_TIME_H     sys/time.h)
cares_extrainclude_ifset (HAVE_SYS_UIO_H      sys/uio.h)
cares_extrainclude_ifset (HAVE_TIME_H         time.h)
cares_extrainclude_ifset (HAVE_FCNTL_H        fcntl.h)
cares_extrainclude_ifset (HAVE_UNISTD_H       unistd.h)
cares_extrainclude_ifset (HAVE_WINSOCK2_H     winsock2.h)
cares_extrainclude_ifset (HAVE_WS2TCPIP_H     ws2tcpip.h)
cares_extrainclude_ifset (HAVE_WINDOWS_H      windows.h)

# Check Types
# CHECK_TYPE_SIZE can't be used to see if a type exists because on Apple when
# building multi-arch, it will throw an error.  So we need to wrap
# CHECK_C_SOURCE_COMPILES for our tests.
macro (cares_type_exists type var)
  set(_CARES_C_SOURCE "
    #include <stdio.h>
    #include <stdlib.h>
  ")
  foreach(_C_HEADER ${CMAKE_EXTRA_INCLUDE_FILES})
    set(_CARES_C_SOURCE "${_CARES_C_SOURCE}
    #include <${_C_HEADER}>")
  endforeach(_C_HEADER)

  set(_CARES_C_SOURCE "${_CARES_C_SOURCE}
    int main() {
      ${type} var_exists;
      (void)var_exists;
      return 0;
    }
  ")
  check_c_source_compiles ("${_CARES_C_SOURCE}" ${var})
endmacro ()

cares_type_exists (socklen_t                 HAVE_SOCKLEN_T)
cares_type_exists (SOCKET                    HAVE_TYPE_SOCKET)
cares_type_exists (bool                      HAVE_BOOL_T)
cares_type_exists (ssize_t                   HAVE_SSIZE_T)
cares_type_exists ("long long"               HAVE_LONGLONG)
cares_type_exists (sig_atomic_t              HAVE_SIG_ATOMIC_T)
cares_type_exists ("struct addrinfo"         HAVE_STRUCT_ADDRINFO)
cares_type_exists ("struct in6_addr"         HAVE_STRUCT_IN6_ADDR)
cares_type_exists ("struct sockaddr_in6"     HAVE_STRUCT_SOCKADDR_IN6)
cares_type_exists ("struct sockaddr_storage" HAVE_STRUCT_SOCKADDR_STORAGE)
cares_type_exists ("struct timeval"          HAVE_STRUCT_TIMEVAL)


# Check for preprocessor defines
check_symbol_exists (AF_INET6        "${CMAKE_EXTRA_INCLUDE_FILES}" HAVE_AF_INET6)
check_symbol_exists (O_NONBLOCK      "${CMAKE_EXTRA_INCLUDE_FILES}" HAVE_O_NONBLOCK)
check_symbol_exists (FIONBIO         "${CMAKE_EXTRA_INCLUDE_FILES}" HAVE_FIONBIO)
check_symbol_exists (SIOCGIFADDR     "${CMAKE_EXTRA_INCLUDE_FILES}" HAVE_IOCTL_SIOCGIFADDR)
check_symbol_exists (MSG_NOSIGNAL    "${CMAKE_EXTRA_INCLUDE_FILES}" HAVE_MSG_NOSIGNAL)
check_symbol_exists (PF_INET6        "${CMAKE_EXTRA_INCLUDE_FILES}" HAVE_PF_INET6)
check_symbol_exists (SO_NONBLOCK     "${CMAKE_EXTRA_INCLUDE_FILES}" HAVE_SO_NONBLOCK)

# XCode v8 bug: iOS when targeting less than v10, or MacOS when targeting less than v10.12 will
# say clock_gettime exists, it is a weak symbol that only exists in iOS10/MacOS10.12 and will
# cause a crash at runtime when running on older versions.  Skip finding CLOCK_MONOTONIC on older
# OS's.
if ((NOT APPLE) OR IOS_V10 OR MACOS_V1012)
  check_symbol_exists (CLOCK_MONOTONIC "${CMAKE_EXTRA_INCLUDE_FILES}" HAVE_CLOCK_GETTIME_MONOTONIC)
endif ()

check_struct_has_member("struct sockaddr_in6" sin6_scope_id "${CMAKE_EXTRA_INCLUDE_FILES}" HAVE_SOCKADDR_IN6_SIN6_SCOPE_ID LANGUAGE C)

# Check for "LL" numeric suffix support
check_c_source_compiles ("int main() { int n=1234LL; return 0; }" HAVE_LL)


check_symbol_exists (bitncmp         "${CMAKE_EXTRA_INCLUDE_FILES}" HAVE_BITNCMP)
check_symbol_exists (closesocket     "${CMAKE_EXTRA_INCLUDE_FILES}" HAVE_CLOSESOCKET)
check_symbol_exists (CloseSocket     "${CMAKE_EXTRA_INCLUDE_FILES}" HAVE_CLOSESOCKET_CAMEL)
check_symbol_exists (connect         "${CMAKE_EXTRA_INCLUDE_FILES}" HAVE_CONNECT)
check_symbol_exists (fcntl           "${CMAKE_EXTRA_INCLUDE_FILES}" HAVE_FCNTL)
check_symbol_exists (freeaddrinfo    "${CMAKE_EXTRA_INCLUDE_FILES}" HAVE_FREEADDRINFO)
check_symbol_exists (getaddrinfo     "${CMAKE_EXTRA_INCLUDE_FILES}" HAVE_GETADDRINFO)
check_symbol_exists (getenv          "${CMAKE_EXTRA_INCLUDE_FILES}" HAVE_GETENV)
check_symbol_exists (gethostbyaddr   "${CMAKE_EXTRA_INCLUDE_FILES}" HAVE_GETHOSTBYADDR)
check_symbol_exists (gethostbyname   "${CMAKE_EXTRA_INCLUDE_FILES}" HAVE_GETHOSTBYNAME)
check_symbol_exists (gethostname     "${CMAKE_EXTRA_INCLUDE_FILES}" HAVE_GETHOSTNAME)
check_symbol_exists (getnameinfo     "${CMAKE_EXTRA_INCLUDE_FILES}" HAVE_GETNAMEINFO)
check_symbol_exists (getservbyport_r "${CMAKE_EXTRA_INCLUDE_FILES}" HAVE_GETSERVBYPORT_R)
check_symbol_exists (gettimeofday    "${CMAKE_EXTRA_INCLUDE_FILES}" HAVE_GETTIMEOFDAY)
check_symbol_exists (if_indextoname  "${CMAKE_EXTRA_INCLUDE_FILES}" HAVE_IF_INDEXTONAME)
check_symbol_exists (inet_net_pton   "${CMAKE_EXTRA_INCLUDE_FILES}" HAVE_INET_NET_PTON)
if (NOT WIN32)
  # Disabled on Windows, because these functions are only really supported on Windows
  # Vista or newer (_WIN32_WINNT >= 0x0600). Older versions of Windows may provide
  # them as experimental non-working features, so we have to disable them manually.
  check_symbol_exists (inet_ntop       "${CMAKE_EXTRA_INCLUDE_FILES}" HAVE_INET_NTOP)
  check_symbol_exists (inet_pton       "${CMAKE_EXTRA_INCLUDE_FILES}" HAVE_INET_PTON)
endif ()
check_symbol_exists (ioctl           "${CMAKE_EXTRA_INCLUDE_FILES}" HAVE_IOCTL)
check_symbol_exists (ioctlsocket     "${CMAKE_EXTRA_INCLUDE_FILES}" HAVE_IOCTLSOCKET)
check_symbol_exists (IoctlSocket     "${CMAKE_EXTRA_INCLUDE_FILES}" HAVE_IOCTLSOCKET_CAMEL)
check_symbol_exists (recv            "${CMAKE_EXTRA_INCLUDE_FILES}" HAVE_RECV)
check_symbol_exists (recvfrom        "${CMAKE_EXTRA_INCLUDE_FILES}" HAVE_RECVFROM)
check_symbol_exists (send            "${CMAKE_EXTRA_INCLUDE_FILES}" HAVE_SEND)
check_symbol_exists (setsockopt      "${CMAKE_EXTRA_INCLUDE_FILES}" HAVE_SETSOCKOPT)
check_symbol_exists (socket          "${CMAKE_EXTRA_INCLUDE_FILES}" HAVE_SOCKET)
check_symbol_exists (strcasecmp      "${CMAKE_EXTRA_INCLUDE_FILES}" HAVE_STRCASECMP)
check_symbol_exists (strcmpi         "${CMAKE_EXTRA_INCLUDE_FILES}" HAVE_STRCMPI)
check_symbol_exists (strdup          "${CMAKE_EXTRA_INCLUDE_FILES}" HAVE_STRDUP)
check_symbol_exists (stricmp         "${CMAKE_EXTRA_INCLUDE_FILES}" HAVE_STRICMP)
check_symbol_exists (strncasecmp     "${CMAKE_EXTRA_INCLUDE_FILES}" HAVE_STRNCASECMP)
check_symbol_exists (strncmpi        "${CMAKE_EXTRA_INCLUDE_FILES}" HAVE_STRNCMPI)
check_symbol_exists (strnicmp        "${CMAKE_EXTRA_INCLUDE_FILES}" HAVE_STRNICMP)
check_symbol_exists (writev          "${CMAKE_EXTRA_INCLUDE_FILES}" HAVE_WRITEV)

# On Android, the system headers may define __system_property_get(), but excluded
# from libc.  We need to perform a link test instead of a header/symbol test.
check_function_exists (__system_property_get HAVE___SYSTEM_PROPERTY_GET)

# Unset temporary data
set (CMAKE_EXTRA_INCLUDE_FILES)
set (CMAKE_REQUIRED_DEFINITIONS)
set (CMAKE_REQUIRED_LIBRARIES)


################################################################################
# recv, recvfrom, send, getnameinfo, gethostname
# ARGUMENTS AND RETURN VALUES
#
# The AutoTools build tries to be really thorough here.  So much so that it
# takes forever.  We really don't want to do that.  Lets make some educated
# guesses based on datatypes we have available, and for others, use some 'sane'
# defaults.  This should be much quicker and nearly as accurate ... and even
# if not, it probably won't matter in the least.

if (HAVE_SSIZE_T AND HAVE_SOCKLEN_T)
  # If we have ssize_t and socklen_t, the API is usually sane and uses ssize_t and size_t for lengths
  set (RECVFROM_TYPE_RETV ssize_t)
  set (RECVFROM_TYPE_ARG3 size_t)
else ()
  set (RECVFROM_TYPE_RETV int)
  set (RECVFROM_TYPE_ARG3 int)
endif ()

if (HAVE_TYPE_SOCKET)
  # If the SOCKET type is defined, it uses socket ... should be windows only
  set (RECVFROM_TYPE_ARG1 SOCKET)
else ()
  set (RECVFROM_TYPE_ARG1 int)
endif()

if (HAVE_SOCKLEN_T)
  # If we have socklen_t the APIs pretty much always actually use it
  set (RECVFROM_TYPE_ARG6 "socklen_t *")
  set (GETNAMEINFO_TYPE_ARG2 socklen_t)
  set (GETNAMEINFO_TYPE_ARG46 socklen_t)
else ()
  set (RECVFROM_TYPE_ARG6 "int *")
  set (GETNAMEINFO_TYPE_ARG2 int)
  set (GETNAMEINFO_TYPE_ARG46 int)
endif ()

# Functions are typically consistent so the equivalent fields map ... equivalently
set (RECV_TYPE_RETV ${RECVFROM_TYPE_RETV})
set (SEND_TYPE_RETV ${RECVFROM_TYPE_RETV})
set (RECV_TYPE_ARG1 ${RECVFROM_TYPE_ARG1})
set (SEND_TYPE_ARG1 ${RECVFROM_TYPE_ARG1})
set (RECV_TYPE_ARG3 ${RECVFROM_TYPE_ARG3})
set (SEND_TYPE_ARG3 ${RECVFROM_TYPE_ARG3})
set (GETHOSTNAME_TYPE_ARG2 ${RECVFROM_TYPE_ARG3})

# These should always be "sane" values to use always
set (RECVFROM_QUAL_ARG5 )
set (RECVFROM_TYPE_ARG2 "void *")
set (RECVFROM_TYPE_ARG4 int)
set (RECVFROM_TYPE_ARG5 "struct sockaddr *")
set (RECV_TYPE_ARG2 "void *")
set (RECV_TYPE_ARG4 int)
set (GETNAMEINFO_TYPE_ARG1 "struct sockaddr *")
set (GETNAMEINFO_TYPE_ARG7 int)
set (SEND_TYPE_ARG2 "void *")
set (SEND_TYPE_ARG4 int)
################################################################################


# HAVE_CXX11  ??
# HAVE_SIG_ATOMIC_T_VOLATILE  ??


# Set a few variables by hand that C-Ares wants, logically, based on detection
# data.

if (HAVE_SOCKLEN_T)
  set (CARES_TYPEOF_ARES_SOCKLEN_T "socklen_t")
else ()
  set (CARES_TYPEOF_ARES_SOCKLEN_T "int")
endif ()

if (HAVE_SSIZE_T)
  set (CARES_TYPEOF_ARES_SSIZE_T "ssize_t")
else ()
  if (WIN32)
    if ("${CMAKE_SIZEOF_VOID_P}" EQUAL "8")
      set (CARES_TYPEOF_ARES_SSIZE_T "__int64")
    else ()
      set (CARES_TYPEOF_ARES_SSIZE_T "int")
    endif ()
  else ()
    set (CARES_TYPEOF_ARES_SSIZE_T "long")
  endif ()
endif ()

if (HAVE_FCNTL AND HAVE_O_NONBLOCK)
  set (HAVE_FCNTL_O_NONBLOCK 1)
endif ()

if (HAVE_IOCTL AND HAVE_FIONBIO)
  set (HAVE_IOCTL_FIONBIO 1)
endif ()

if (HAVE_IOCTLSOCKET AND HAVE_FIONBIO)
  set (HAVE_IOCTLSOCKET_FIONBIO 1)
endif ()

if (HAVE_IOCTLSOCKET_CAMEL AND HAVE_FIONBIO)
  set (HAVE_IOCTLSOCKET_CAMEL_FIONBIO 1)
endif ()

if (HAVE_GETADDRINFO)
  if (CMAKE_SYSTEM_NAME STREQUAL "Darwin" OR
    CMAKE_SYSTEM_NAME STREQUAL "HPUX" OR
    CMAKE_SYSTEM_NAME STREQUAL "NetBSD" OR
    CMAKE_SYSTEM_NAME STREQUAL "SunOS" OR
    CMAKE_SYSTEM_NAME STREQUAL "FreeBSD" OR
    CMAKE_SYSTEM_NAME STREQUAL "AIX" OR
    WIN32)
      set (HAVE_GETADDRINFO_THREADSAFE 1)
  endif ()
endif ()

if (HAVE_TIME_H AND HAVE_SYS_TIME_H)
  set (TIME_WITH_SYS_TIME 1)
endif ()

if (HAVE_GETSERVBYPORT_R)
  # TODO : Should probably autodetect
  if (CMAKE_SYSTEM_NAME STREQUAL "SunOS")
    set (GETSERVBYPORT_R_ARGS 5)
  elseif (CMAKE_SYSTEM_NAME STREQUAL "AIX" OR
    CMAKE_SYSTEM_NAME STREQUAL "OpenBSD")
    set (GETSERVBYPORT_R_ARGS 4)
  else ()
    # Probably linux
    set (GETSERVBYPORT_R_ARGS 6)
  endif ()
endif ()

# Set some aliases used for ares_build.h
if (HAVE_SYS_TYPES_H)
  set (CARES_HAVE_SYS_TYPES_H 1)
endif ()
if (HAVE_SYS_SOCKET_H)
  set (CARES_HAVE_SYS_SOCKET_H 1)
endif()
if (HAVE_WS2TCPIP_H)
  set (CARES_HAVE_WS2TCPIP_H 1)
endif()
if (HAVE_WINSOCK2_H)
  set (CARES_HAVE_WINSOCK2_H 1)
endif()
if (HAVE_WINDOWS_H)
  set (CARES_HAVE_WINDOWS_H 1)
endif()

# Write ares_build.h configuration file.  This is an installed file.
configure_file (ares_build.h.cmake ${PROJECT_BINARY_DIR}/ares_build.h)

# Write ares_config.h configuration file.  This is used only for the build.
configure_file (ares_config.h.cmake ${PROJECT_BINARY_DIR}/ares_config.h)


# transform_makefile_inc
#
# This function consumes the "Makefile.inc" autotools file, and converts it into
#  "Makefile.inc.cmake", a cmake include file; transforming this:
#
# CSOURCES = ares__close_sockets.c  \
#   ares__get_hostent.c      \
#   ares__read_line.c      \
#   ...
#
#   into this:
#
# set (CSOURCES
#   ares__close_sockets.c
#   ares__get_hostent.c
#   ares__read_line.c
#  ...
function(transform_makefile_inc INPUT_FILE OUTPUT_FILE)
  file(READ ${INPUT_FILE} MAKEFILE_INC_TEXT)
  string(REPLACE "$(top_srcdir)"   "\${PROJECT_SOURCE_DIR}" MAKEFILE_INC_TEXT ${MAKEFILE_INC_TEXT})
  string(REPLACE "$(top_builddir)" "\${PROJECT_BINARY_DIR}" MAKEFILE_INC_TEXT ${MAKEFILE_INC_TEXT})

  string(REGEX REPLACE "\\\\\n" "ß!ß" MAKEFILE_INC_TEXT ${MAKEFILE_INC_TEXT})
  string(REGEX REPLACE "([a-zA-Z_][a-zA-Z0-9_]*)[\t ]*=[\t ]*([^\n]*)" "set(\\1 \\2)" MAKEFILE_INC_TEXT ${MAKEFILE_INC_TEXT})
  string(REPLACE "ß!ß" "\n" MAKEFILE_INC_TEXT ${MAKEFILE_INC_TEXT})

  string(REGEX REPLACE "\\$\\(([a-zA-Z_][a-zA-Z0-9_]*)\\)" "\${\\1}" MAKEFILE_INC_TEXT ${MAKEFILE_INC_TEXT})    # Replace $() with ${}
  string(REGEX REPLACE "@([a-zA-Z_][a-zA-Z0-9_]*)@" "\${\\1}" MAKEFILE_INC_TEXT ${MAKEFILE_INC_TEXT})    # Replace @@ with ${}, even if that may not be read by CMake scripts.
  file(WRITE ${OUTPUT_FILE} ${MAKEFILE_INC_TEXT})
endfunction()

# run the function...
transform_makefile_inc("Makefile.inc" "${PROJECT_BINARY_DIR}/Makefile.inc.cmake")
include(${PROJECT_BINARY_DIR}/Makefile.inc.cmake)



# Build the dynamic/shared library
if (BUILD_SHARED_LIBS)
  add_library (${PROJECT_NAME} SHARED ${CSOURCES})

  # Convert CARES_LIB_VERSIONINFO libtool version format into VERSION and SOVERSION
  # Convert from ":" separated into CMake list format using ";"
  string (REPLACE ":" ";" CARES_LIB_VERSIONINFO ${CARES_LIB_VERSIONINFO})
  list (GET CARES_LIB_VERSIONINFO 0 CARES_LIB_VERSION_CURRENT)
  list (GET CARES_LIB_VERSIONINFO 1 CARES_LIB_VERSION_REVISION)
  list (GET CARES_LIB_VERSIONINFO 2 CARES_LIB_VERSION_AGE)
  math (EXPR CARES_LIB_VERSION_MAJOR "${CARES_LIB_VERSION_CURRENT} - ${CARES_LIB_VERSION_AGE}")
  set  (CARES_LIB_VERSION_MINOR "${CARES_LIB_VERSION_AGE}")
  set  (CARES_LIB_VERSION_RELEASE "${CARES_LIB_VERSION_REVISION}")

  set_target_properties (${PROJECT_NAME} PROPERTIES
    EXPORT_NAME cares
    OUTPUT_NAME cares
    VERSION     "${CARES_LIB_VERSION_MAJOR}.${CARES_LIB_VERSION_MINOR}.${CARES_LIB_VERSION_RELEASE}"
    SOVERSION   "${CARES_LIB_VERSION_MAJOR}"
  )

  target_include_directories (${PROJECT_NAME} PUBLIC "$<BUILD_INTERFACE:${PROJECT_BINARY_DIR}>")
  target_include_directories (${PROJECT_NAME} PUBLIC "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}>")

  target_compile_definitions (${PROJECT_NAME} PRIVATE HAVE_CONFIG_H=1 CARES_BUILDING_LIBRARY)

  target_link_libraries (${PROJECT_NAME} PUBLIC ${CARES_DEPENDENT_LIBS})

  if (CARES_INSTALL)
    install (TARGETS ${PROJECT_NAME} EXPORT ${PROJECT_NAME}-targets
      ${TARGETS_INST_DEST}
    )
  endif ()
  set (STATIC_SUFFIX "_static")

  # For chain building: add alias targets that look like import libs that would be returned by find_package(c-ares).
  add_library (${PROJECT_NAME}::cares_shared ALIAS ${PROJECT_NAME})
  add_library (${PROJECT_NAME}::cares        ALIAS ${PROJECT_NAME})
endif ()

# Build the static library
if (NOT BUILD_SHARED_LIBS)
  set (LIBNAME ${PROJECT_NAME}${STATIC_SUFFIX})

  add_library (${LIBNAME} STATIC ${CSOURCES})

  set_target_properties (${LIBNAME} PROPERTIES
    EXPORT_NAME cares${STATIC_SUFFIX}
    OUTPUT_NAME cares${STATIC_SUFFIX}
  )

  if (CARES_STATIC_PIC)
    set_target_properties (${LIBNAME} PROPERTIES POSITION_INDEPENDENT_CODE True)
  endif ()

  target_include_directories (${LIBNAME} PUBLIC "$<BUILD_INTERFACE:${PROJECT_BINARY_DIR}>")
  target_include_directories (${LIBNAME} PUBLIC "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}>")

  target_compile_definitions (${LIBNAME}
    PUBLIC  CARES_STATICLIB
    PRIVATE HAVE_CONFIG_H=1
  )

  target_link_libraries (${LIBNAME} PUBLIC ${CARES_DEPENDENT_LIBS})
  if (CARES_INSTALL)
    install (TARGETS ${LIBNAME} EXPORT ${PROJECT_NAME}-targets
      ${TARGETS_INST_DEST}
    )
  endif ()

  # For chain building: add alias targets that look like import libs that would be returned by find_package(c-ares).
  add_library (${PROJECT_NAME}::cares_static ALIAS ${LIBNAME})
  if (NOT TARGET ${PROJECT_NAME}::cares)
    # Only use static for the generic alias if shared lib wasn't built.
    add_library (${PROJECT_NAME}::cares ALIAS ${LIBNAME})
  endif ()
endif ()

if (CARES_BUILD_TESTS)
  enable_testing ()
  add_subdirectory (test)
endif ()

# Headers installation target
if (CARES_INSTALL)
  set (CARES_HEADERS ares.h ares_version.h ares_dns.h "${PROJECT_BINARY_DIR}/ares_build.h" ares_rules.h)
  install (FILES ${CARES_HEADERS} DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
endif ()

# Export targets
if (CARES_INSTALL)
  set (CMAKECONFIG_INSTALL_DIR "${CMAKE_INSTALL_LIBDIR}/cmake/${PROJECT_NAME}")
  include (CMakePackageConfigHelpers)
  configure_package_config_file (${PROJECT_NAME}-config.cmake.in ${PROJECT_NAME}-config.cmake
    INSTALL_DESTINATION ${CMAKECONFIG_INSTALL_DIR}
    PATH_VARS CMAKE_INSTALL_INCLUDEDIR
    NO_CHECK_REQUIRED_COMPONENTS_MACRO
  )
  install (EXPORT ${PROJECT_NAME}-targets DESTINATION ${CMAKECONFIG_INSTALL_DIR} NAMESPACE ${PROJECT_NAME}::)
  install (FILES "${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}-config.cmake" DESTINATION ${CMAKECONFIG_INSTALL_DIR})

  # pkgconfig support
  if (NOT BUILD_SHARED_LIBS)
    set (CPPFLAG_CARES_STATICLIB "-DCARES_STATICLIB")
    foreach (LIB ${CARES_DEPENDENT_LIBS})
      set (CARES_PRIVATE_LIBS "${CARES_PRIVATE_LIBS} -l${LIB}")
    endforeach ()
  endif ()
  configure_file("libcares.pc.cmake" "libcares.pc" @ONLY)
  install (FILES "${CMAKE_CURRENT_BINARY_DIR}/libcares.pc" DESTINATION "${CMAKE_INSTALL_LIBDIR}/pkgconfig")
endif ()

# Legacy chain-building variables (provided for compatibility with old code).
# Don't use these, external code should be updated to refer to the aliases directly (e.g., Cares::cares).
set (CARES_FOUND 1 CACHE INTERNAL "CARES LIBRARY FOUND")
set (CARES_LIBRARIES ${PROJECT_NAME}::cares CACHE INTERNAL "CARES LIBRARIES")

if(CARES_BUILD_TOOLS)
  # Build ahost
  add_executable (ahost ahost.c ${SAMPLESOURCES})
  target_compile_definitions (ahost PRIVATE HAVE_CONFIG_H=1)
  target_link_libraries (ahost PRIVATE ${PROJECT_NAME})
  if (CARES_INSTALL)
    install (TARGETS ahost ${TARGETS_INST_DEST})
  endif ()


  # Build adig
  add_executable (adig adig.c ${SAMPLESOURCES})
  target_compile_definitions (adig PRIVATE HAVE_CONFIG_H=1)
  target_link_libraries (adig PRIVATE ${PROJECT_NAME})
  if (CARES_INSTALL)
    install (TARGETS adig ${TARGETS_INST_DEST})
  endif ()


  # Build acountry
  add_executable (acountry acountry.c ${SAMPLESOURCES})
  target_compile_definitions (acountry PRIVATE HAVE_CONFIG_H=1)
  target_link_libraries (acountry PRIVATE ${PROJECT_NAME})
  if (CARES_INSTALL)
    install (TARGETS acountry ${TARGETS_INST_DEST})
  endif ()
endif()
