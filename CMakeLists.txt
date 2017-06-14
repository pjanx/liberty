project (liberty C)
cmake_minimum_required (VERSION 2.8.5)

# Moar warnings
if ("${CMAKE_C_COMPILER_ID}" MATCHES "GNU" OR CMAKE_COMPILER_IS_GNUC)
	# -Wunused-function is pretty annoying here, as everything is static
	set (CMAKE_C_FLAGS "-std=c99 -Wall -Wextra -Wno-unused-function")
endif ("${CMAKE_C_COMPILER_ID}" MATCHES "GNU" OR CMAKE_COMPILER_IS_GNUC)

# Dependencies
set (CMAKE_MODULE_PATH ${PROJECT_SOURCE_DIR}/cmake)
include (AddThreads)

find_package (PkgConfig REQUIRED)
pkg_check_modules (libssl REQUIRED libssl libcrypto)

if ("${CMAKE_SYSTEM_NAME}" MATCHES "BSD")
	include_directories (/usr/local/include)
	link_directories (/usr/local/lib)
	# Our POSIX version macros make these undefined
	add_definitions (-D__BSD_VISIBLE=1 -D_BSD_SOURCE=1)
endif ("${CMAKE_SYSTEM_NAME}" MATCHES "BSD")

set (common_libraries ${libssl_LIBRARIES})
include_directories (${libssl_INCLUDE_DIRS})
link_directories (${libssl_LIBRARY_DIRS})

# -lrt is only for glibc < 2.17
# -liconv may or may not be a part of libc
foreach (extra iconv rt)
	find_library (extra_lib_${extra} ${extra})
	if (extra_lib_${extra})
		list (APPEND common_libraries ${extra})
	endif (extra_lib_${extra})
endforeach (extra)

# Build some unit tests
include_directories (${PROJECT_SOURCE_DIR})
enable_testing ()
foreach (name liberty proto)
	add_executable (test-${name} tests/${name}.c ${common_sources})
	add_threads (test-${name})
	target_link_libraries (test-${name} ${common_libraries})
	add_test (NAME test-${name} COMMAND test-${name})
endforeach (name)
