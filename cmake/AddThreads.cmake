# Public Domain

# We're looking for pthreads only, while preferring the -pthread flag
set (CMAKE_THREAD_PREFER_PTHREAD ON)
set (THREADS_PREFER_PTHREAD_FLAG ON)
find_package (Threads)

# Prepares the given target for threads
function (add_threads target)
	if (NOT Threads_FOUND OR NOT CMAKE_USE_PTHREADS_INIT)
		message (FATAL_ERROR "pthreads not found")
	endif ()

	if (THREADS_HAVE_PTHREAD_ARG)
		set_property (TARGET ${target} PROPERTY
			COMPILE_OPTIONS "-pthread")
		set_property (TARGET ${target} PROPERTY
			INTERFACE_COMPILE_OPTIONS "-pthread")
	endif ()
	if (CMAKE_THREAD_LIBS_INIT)
		target_link_libraries (${target} "${CMAKE_THREAD_LIBS_INIT}")
	endif ()
endfunction ()
