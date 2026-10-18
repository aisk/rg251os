# Injected into mGBA's top-level project via CMAKE_PROJECT_mGBA_INCLUDE.
# mGBA hardcodes the libretro core as a SHARED module, so once its top-level
# CMakeLists.txt has finished, add a STATIC twin built from the same sources
# and target definitions. Directory-level include paths and definitions apply
# automatically because the deferred call runs in mGBA's source directory.

function(rg_add_libretro_static)
	set(core ${BINARY_NAME}_libretro)
	if(NOT TARGET ${core})
		message(FATAL_ERROR "${core} target missing, is BUILD_LIBRETRO on?")
	endif()
	get_target_property(srcs ${core} SOURCES)
	get_target_property(defs ${core} COMPILE_DEFINITIONS)
	add_library(${core}_static STATIC ${srcs})
	add_dependencies(${core}_static ${BINARY_NAME}-version-info)
	set_target_properties(${core}_static PROPERTIES
		OUTPUT_NAME ${core}
		COMPILE_DEFINITIONS "${defs}")
endfunction()

cmake_language(DEFER CALL rg_add_libretro_static)
