# Public Domain

find_path (Unistring_INCLUDE_DIRS unistr.h)
find_library (Unistring_LIBRARIES NAMES unistring libunistring)

include (FindPackageHandleStandardArgs)
FIND_PACKAGE_HANDLE_STANDARD_ARGS (Unistring DEFAULT_MSG
	Unistring_INCLUDE_DIRS Unistring_LIBRARIES)

mark_as_advanced (Unistring_LIBRARIES Unistring_INCLUDE_DIRS)
