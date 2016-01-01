project (liberty C)
cmake_minimum_required (VERSION 2.8.5)

# Moar warnings
if ("${CMAKE_C_COMPILER_ID}" MATCHES "GNU" OR CMAKE_COMPILER_IS_GNUC)
	# -Wunused-function is pretty annoying here, as everything is static
	set (CMAKE_C_FLAGS "-std=c99 -Wall -Wextra -Wno-unused-function")
endif ("${CMAKE_C_COMPILER_ID}" MATCHES "GNU" OR CMAKE_COMPILER_IS_GNUC)

# Dependencies
find_package (PkgConfig REQUIRED)
pkg_check_modules (libssl REQUIRED libssl libcrypto)

set (CMAKE_MODULE_PATH ${PROJECT_SOURCE_DIR}/cmake)
include (AddThreads)

# -lrt is only for glibc < 2.17
set (common_libraries ${libssl_LIBRARIES} rt)
include_directories (${libssl_INCLUDE_DIRS})
link_directories (${libssl_LIBRARY_DIRS})

# Generate a configuration file
# TODO: actualy use the configuration file for something; so far we allow
#   for direct inclusion without running this CMakeLists.txt
configure_file (${PROJECT_SOURCE_DIR}/liberty-config.h.in
	${PROJECT_BINARY_DIR}/liberty-config.h)
include_directories (${PROJECT_SOURCE_DIR} ${PROJECT_BINARY_DIR})
set (common_sources ${PROJECT_BINARY_DIR}/liberty-config.h)

# Build some unit tests
enable_testing ()
foreach (name liberty proto)
	add_executable (test-${name} tests/${name}.c ${common_sources})
	add_threads (test-${name})
	target_link_libraries (test-${name} ${common_libraries})
	add_test (NAME test-${name} COMMAND test-${name})
endforeach (name)
