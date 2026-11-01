make_release_only()

# Declare the option (in case it wasn't defined in the root CMakeLists.txt)
option(MadeForWinXP "Enable compatibility configurations for Windows XP" OFF)

if (MSVC)
	set( CMAKE_C_FLAGS "${CMAKE_C_FLAGS} /wd4244" )
endif()

add_definitions( -DBZ_NO_STDIO )

add_library( bz2 STATIC
    blocksort.c
    bzlib.c
    compress.c
    crctable.c
    decompress.c
    huffman.c
    randtable.c )
link_libraries("-static")

# ==================================== XP Patch - START ===========================
if (MadeForWinXP)
	message(STATUS "[bz2] MadeForWinXP preset is ENABLED. Applying PCH disable patch...")
	
	# Get the list of all source files belonging to bz2 target
	get_target_property(BZ2_SOURCES bz2 SOURCES)
	foreach(source_file ${BZ2_SOURCES})
		# Force clear precompiled header flags and set explicit Windows XP targeting (0x0501)
		set_source_files_properties(${source_file} PROPERTIES 
			COMPILE_FLAGS "/Y-"
			COMPILE_DEFINITIONS "WIN32_LEAN_AND_MEAN;_WIN32_WINNT=0x0501;WINVER=0x0501"
		)
	endforeach()
endif()
# ==================================== XP Patch - FINISH ==========================

target_link_libraries( bz2 )
