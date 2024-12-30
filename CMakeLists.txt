cmake_minimum_required (VERSION 3.0...3.27)
project (liberty C CXX)

# Moar warnings
set (CMAKE_CXX_STANDARD 11)
if ("${CMAKE_C_COMPILER_ID}" MATCHES "GNU" OR CMAKE_COMPILER_IS_GNUCC)
	# -Wunused-function is pretty annoying here, as everything is static
	set (wdisabled "-Wno-unused-function")
	set (CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -std=c99 -Wall -Wextra ${wdisabled}")
endif ()

# Dependencies
set (CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")
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
		list (APPEND common_libraries ${extra_lib_${extra}})
	endif ()
endforeach ()

# Build some unit tests
include_directories ("${PROJECT_SOURCE_DIR}")
enable_testing ()
set (tests liberty proto xdg)

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

# --- Tools --------------------------------------------------------------------

# Test the AsciiDoc manual page generator for a successful parse
set (ASCIIMAN "${PROJECT_SOURCE_DIR}/tools/asciiman.awk")
add_custom_command (OUTPUT libertyxdr.7
	COMMAND env LC_ALL=C awk -f ${ASCIIMAN}
		"${PROJECT_SOURCE_DIR}/libertyxdr.adoc" > libertyxdr.7
	DEPENDS libertyxdr.adoc ${ASCIIMAN}
	COMMENT "Generating man page for libertyxdr" VERBATIM)
add_custom_target (docs ALL DEPENDS libertyxdr.7)

# Test the --help/--version to AsciiDoc convertor
add_test (test-help2adoc
	env LC_ALL=C "${PROJECT_SOURCE_DIR}/tests/help2adoc.sh")

# Test CMake script parsing
add_test (test-cmake-parser
	env LC_ALL=C awk -f "${PROJECT_SOURCE_DIR}/tools/cmake-parser.awk"
	-f "${PROJECT_SOURCE_DIR}/tools/cmake-dump.awk" ${CMAKE_CURRENT_LIST_FILE})

# Test protocol code generation
set (lxdrgen_outputs)
set (lxdrgen_base "${PROJECT_BINARY_DIR}/lxdrgen.lxdr")
foreach (backend c cpp go mjs swift)
	list (APPEND lxdrgen_outputs ${lxdrgen_base}.${backend})
	add_custom_command (OUTPUT ${lxdrgen_base}.${backend}
		COMMAND env LC_ALL=C awk
			-f "${PROJECT_SOURCE_DIR}/tools/lxdrgen.awk"
			-f "${PROJECT_SOURCE_DIR}/tools/lxdrgen-${backend}.awk"
			-v PrefixCamel=ProtoGen
			"${PROJECT_SOURCE_DIR}/tests/lxdrgen.lxdr"
			> ${lxdrgen_base}.${backend}
		DEPENDS
			"${PROJECT_SOURCE_DIR}/tools/lxdrgen.awk"
			"${PROJECT_SOURCE_DIR}/tools/lxdrgen-${backend}.awk"
			"${PROJECT_SOURCE_DIR}/tests/lxdrgen.lxdr"
		COMMENT "Generating test protocol code (${backend})" VERBATIM)
endforeach ()
add_custom_target (test-lxdrgen-outputs ALL DEPENDS ${lxdrgen_outputs})

set_source_files_properties (${lxdrgen_base}.c
	PROPERTIES HEADER_FILE_ONLY TRUE)
add_executable (test-lxdrgen-c tests/lxdrgen.c ${lxdrgen_base}.c)
target_include_directories (test-lxdrgen-c PUBLIC ${PROJECT_BINARY_DIR})
add_test (NAME test-lxdrgen-c COMMAND test-lxdrgen-c)

set_source_files_properties (${lxdrgen_base}.cpp
	PROPERTIES HEADER_FILE_ONLY TRUE)
if (WIN32)
	add_executable (test-lxdrgen-cpp tests/lxdrgen.cpp
		${lxdrgen_base}.cpp tools/lxdrgen-cpp-win32.cpp)
else ()
	add_executable (test-lxdrgen-cpp tests/lxdrgen.cpp
		${lxdrgen_base}.cpp tools/lxdrgen-cpp-posix.cpp)
endif ()
target_link_libraries (test-lxdrgen-cpp ${common_libraries})
target_include_directories (test-lxdrgen-cpp PUBLIC ${PROJECT_BINARY_DIR})
add_test (NAME test-lxdrgen-cpp COMMAND test-lxdrgen-cpp)

find_program (GO_EXECUTABLE go)
if (GO_EXECUTABLE)
	add_test (test-lxdrgen-go ${GO_EXECUTABLE} vet ${lxdrgen_base}.go)
else ()
	message (WARNING "Cannot test generated protocol code for Go")
endif ()

find_program (NODE_EXECUTABLE node)
if (NODE_EXECUTABLE)
	add_test (test-lxdrgen-mjs ${NODE_EXECUTABLE} -c ${lxdrgen_base}.mjs)
else ()
	message (WARNING "Cannot test generated protocol code for Javascript")
endif ()

find_program (SWIFTC_EXECUTABLE swiftc)
if (SWIFTC_EXECUTABLE)
	add_test (test-lxdrgen-swift
		${SWIFTC_EXECUTABLE} -typecheck ${lxdrgen_base}.swift)
else ()
	message (WARNING "Cannot test generated protocol code for Swift")
endif ()
