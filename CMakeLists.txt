project (liberty C)
cmake_minimum_required (VERSION 2.8.12)

# Moar warnings
if ("${CMAKE_C_COMPILER_ID}" MATCHES "GNU" OR CMAKE_COMPILER_IS_GNUCC)
	# -Wunused-function is pretty annoying here, as everything is static
	set (wdisabled "-Wno-unused-function")
	set (CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -std=c99 -Wall -Wextra ${wdisabled}")
endif ()

# Dependencies
set (CMAKE_MODULE_PATH ${PROJECT_SOURCE_DIR}/cmake)
include (AddThreads)

find_package (PkgConfig REQUIRED)
pkg_check_modules (libssl REQUIRED libssl libcrypto)

if ("${CMAKE_SYSTEM_NAME}" MATCHES "BSD")
	# Our POSIX version macros make these undefined
	add_definitions (-D__BSD_VISIBLE=1 -D_BSD_SOURCE=1)
endif ()

set (common_libraries ${libssl_LIBRARIES})
include_directories (${libssl_INCLUDE_DIRS})
link_directories (${libssl_LIBRARY_DIRS})

# -lrt is only for glibc < 2.17
# -liconv may or may not be a part of libc
foreach (extra iconv rt)
	find_library (extra_lib_${extra} ${extra})
	if (extra_lib_${extra})
		list (APPEND common_libraries ${extra})
	endif ()
endforeach ()

# Build some unit tests
include_directories (${PROJECT_SOURCE_DIR})
enable_testing ()
set (tests liberty proto)

pkg_check_modules (libpulse libpulse)
if (libpulse_FOUND)
	list (APPEND tests pulse)
	list (APPEND common_libraries ${libpulse_LIBRARIES})
	include_directories (${libpulse_INCLUDE_DIRS})
	link_directories (${libpulse_LIBRARY_DIRS})
endif ()

foreach (name ${tests})
	add_executable (test-${name} tests/${name}.c ${common_sources})
	add_threads (test-${name})
	target_link_libraries (test-${name} ${common_libraries})
	add_test (NAME test-${name} COMMAND test-${name})
endforeach ()
