# Print this machine's logical core count on stdout: `cmake -P cmake/HostCores.cmake`.
#
# CI passes it to `cmake --build -j` so a build uses whatever runner it lands on. A bare `-j` is not
# safe there: under Makefiles it means no limit, and every compile starts at once.
cmake_host_system_information(RESULT _cores QUERY NUMBER_OF_LOGICAL_CORES)
execute_process(COMMAND "${CMAKE_COMMAND}" -E echo "${_cores}")
