cmake_minimum_required (VERSION 3.18)
project (wdye VERSION 1 DESCRIPTION "What did you expect?" LANGUAGES C)

set (CMAKE_C_STANDARD 99)
set (CMAKE_C_STANDARD_REQUIRED ON)
set (CMAKE_C_EXTENSIONS OFF)

# -Wunused-function is pretty annoying here, as everything is static
set (options -Wall -Wextra -Wno-unused-function)
add_compile_options ("$<$<CXX_COMPILER_ID:GNU>:${options}>")
add_compile_options ("$<$<CXX_COMPILER_ID:Clang>:${options}>")

set (CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/../../cmake")

find_package (Curses)
find_package (PkgConfig REQUIRED)
pkg_search_module (lua REQUIRED
	lua53 lua5.3 lua-5.3 lua54 lua5.4 lua-5.4 lua>=5.3)

option (WITH_CURSES "Offer terminal sequences using Curses" "${CURSES_FOUND}")

include_directories ("${PROJECT_BINARY_DIR}")
file (CONFIGURE OUTPUT "${PROJECT_BINARY_DIR}/config.h" CONTENT [[
#define PROGRAM_NAME "${PROJECT_NAME}"
#define PROGRAM_VERSION "${PROJECT_VERSION}"
#cmakedefine WITH_CURSES
]])

add_executable (wdye wdye.c)
target_include_directories (wdye PUBLIC ${lua_INCLUDE_DIRS})
target_link_directories (wdye PUBLIC ${lua_LIBRARY_DIRS})
target_link_libraries (wdye PUBLIC ${lua_LIBRARIES})
if (WITH_CURSES)
	target_include_directories (wdye PUBLIC ${CURSES_INCLUDE_DIRS})
	target_link_libraries (wdye PUBLIC ${CURSES_LIBRARIES})
endif ()

add_test (NAME simple COMMAND wdye "${PROJECT_SOURCE_DIR}/test.lua")
include (CTest)
