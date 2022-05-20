cmake_minimum_required(VERSION 3.12)

find_package(Git QUIET)
if(GIT_FOUND)
	execute_process(
		COMMAND ${GIT_EXECUTABLE} describe --tags --dirty
		WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
		OUTPUT_VARIABLE GIT_VERSION
		OUTPUT_STRIP_TRAILING_WHITESPACE
	)
	string(REGEX REPLACE "^v?([0-9]+\\.[0-9]+(\\.[0-9]+)?).*\$" "\\1" GIT_VERSION_TRIMMED "${GIT_VERSION}")
else()
	set(GIT_VERSION "unknown-rev")
	set(GIT_VERSION_TRIMMED "0.0.0")
endif()

project(pageant-ssh-agent-pipe-proxy VERSION "${GIT_VERSION_TRIMMED}" LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)

add_executable(pageant-pipe-proxy pageant-pipe-proxy.cpp)
target_compile_definitions(pageant-pipe-proxy PUBLIC _UNICODE UNICODE)

add_executable(ssh-agent-pipe-proxy pipe-ssh-agent-unix.cpp)
target_compile_definitions(ssh-agent-pipe-proxy PUBLIC _UNICODE UNICODE)
target_link_libraries(ssh-agent-pipe-proxy PRIVATE ws2_32.lib)

install(TARGETS pageant-pipe-proxy ssh-agent-pipe-proxy)


set(CPACK_GENERATOR ZIP)
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY OFF)
set(CPACK_STRIP_FILES ON)
set(CPACK_PACKAGE_VERSION ${GIT_VERSION})


include(CPack)
