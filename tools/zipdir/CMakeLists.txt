#CMakeLists.txt zipdir
cmake_minimum_required( VERSION 3.16 )

if( NOT CMAKE_CROSSCOMPILING )
	include_directories( SYSTEM "${BZIP2_INCLUDE_DIR}" "${LZMA_INCLUDE_DIR}" )
	
	# Explicitly collect source files into a variable
	set( ZIPDIR_SRCS zipdir.c )

	add_executable( zipdir ${ZIPDIR_SRCS} )

	# ==== XP Patch: Force Windows XP environment targeting and completely drop PCH for zipdir ====
	if(MadeForWinXP AND MSVC)
		message(STATUS "[zipdir] MadeForWinXP preset is ENABLED. Forcing clean compatibility flags pass...")
		foreach(source_file ${ZIPDIR_SRCS})
			set_source_files_properties(${source_file} PROPERTIES
				COMPILE_FLAGS "/Y-"
				COMPILE_DEFINITIONS "WIN32_LEAN_AND_MEAN;_WIN32_WINNT=0x0501;WINVER=0x0501;IKNOW_UNKNOWN"
			)
		endforeach()
	endif()
	# =============================================================================================

	target_link_libraries( zipdir miniz ${BZIP2_LIBRARIES} lzma )
	set( CROSS_EXPORTS ${CROSS_EXPORTS} zipdir PARENT_SCOPE )
endif()
