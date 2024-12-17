# Public Domain

function (icon_to_png name svg size output_dir output)
	set (_dimensions "${size}x${size}")
	set (_png_path "${output_dir}/hicolor/${_dimensions}/apps")
	set (_png "${_png_path}/${name}.png")
	set (${output} "${_png}" PARENT_SCOPE)

	set (_find_program_REQUIRE)
	if (NOT ${CMAKE_VERSION} VERSION_LESS 3.18.0)
		set (_find_program_REQUIRE REQUIRED)
	endif ()

	find_program (rsvg_convert_EXECUTABLE rsvg-convert ${_find_program_REQUIRE})
	add_custom_command (OUTPUT "${_png}"
		COMMAND ${CMAKE_COMMAND} -E make_directory "${_png_path}"
		COMMAND ${rsvg_convert_EXECUTABLE} "--output=${_png}"
			"--width=${size}" "--height=${size}" -- "${svg}"
		DEPENDS "${svg}"
		COMMENT "Generating ${name} ${_dimensions} application icon" VERBATIM)
endfunction ()

# You should include a 256x256 icon--which takes less space as raw PNG.
function (icon_for_win32 ico pngs pngs_raw)
	set (_raws)
	foreach (png ${pngs_raw})
		list (APPEND _raws "--raw=${png}")
	endforeach ()

	set (_find_program_REQUIRE)
	if (NOT ${CMAKE_VERSION} VERSION_LESS 3.18.0)
		set (_find_program_REQUIRE REQUIRED)
	endif ()

	find_program (icotool_EXECUTABLE icotool ${_find_program_REQUIRE})
	add_custom_command (OUTPUT "${ico}"
		COMMAND ${icotool_EXECUTABLE} -c -o "${ico}" ${_raws} -- ${pngs}
		DEPENDS ${pngs} ${pngs_raw}
		COMMENT "Generating Windows program icon" VERBATIM)
endfunction ()

function (icon_to_iconset_size name svg size iconset outputs)
	math (EXPR _size2x "${size} * 2")
	set (_dimensions "${size}x${size}")
	set (_png1x "${iconset}/icon_${_dimensions}.png")
	set (_png2x "${iconset}/icon_${_dimensions}@2x.png")
	set (${outputs} "${_png1x};${_png2x}" PARENT_SCOPE)

	set (_find_program_REQUIRE)
	if (NOT ${CMAKE_VERSION} VERSION_LESS 3.18.0)
		set (_find_program_REQUIRE REQUIRED)
	endif ()

	find_program (rsvg_convert_EXECUTABLE rsvg-convert ${_find_program_REQUIRE})
	add_custom_command (OUTPUT "${_png1x}" "${_png2x}"
		COMMAND ${CMAKE_COMMAND} -E make_directory "${iconset}"
		COMMAND ${rsvg_convert_EXECUTABLE} "--output=${_png1x}"
			"--width=${size}" "--height=${size}" -- "${svg}"
		COMMAND ${rsvg_convert_EXECUTABLE} "--output=${_png2x}"
			"--width=${_size2x}" "--height=${_size2x}" -- "${svg}"
		DEPENDS "${svg}"
		COMMENT "Generating ${name} ${_dimensions} icons" VERBATIM)
endfunction ()
function (icon_to_icns svg output_basename output)
	get_filename_component (_name "${output_basename}" NAME_WE)
	set (_iconset "${PROJECT_BINARY_DIR}/${_name}.iconset")
	set (_icon "${PROJECT_BINARY_DIR}/${output_basename}")
	set (${output} "${_icon}" PARENT_SCOPE)

	set (_icon_png_list)
	foreach (_icon_size 16 32 128 256 512)
		icon_to_iconset_size ("${_name}" "${svg}"
			"${_icon_size}" "${_iconset}" _icon_pngs)
		list (APPEND _icon_png_list ${_icon_pngs})
	endforeach ()

	# XXX: This will not normally work from within Nix.
	add_custom_command (OUTPUT "${_icon}"
		COMMAND iconutil -c icns -o "${_icon}" "${_iconset}"
		DEPENDS ${_icon_png_list}
		COMMENT "Generating ${_name} icon" VERBATIM)
	set_source_files_properties ("${_icon}" PROPERTIES
		MACOSX_PACKAGE_LOCATION Resources)
endfunction ()
