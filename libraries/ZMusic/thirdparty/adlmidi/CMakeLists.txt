cmake_minimum_required( VERSION 3.16 )

make_release_only()
use_fast_math()

add_definitions(-DADLMIDI_DISABLE_MIDI_SEQUENCER)

# Collect all source files into a variable
set( ADL_SRCS
	adldata.cpp
	adlmidi.cpp
	adlmidi_load.cpp
	adlmidi_midiplay.cpp
	adlmidi_opl3.cpp
	adlmidi_private.cpp
	chips/dosbox/dbopl.cpp
	chips/dosbox_opl3.cpp
	chips/nuked/nukedopl3_174.c
	chips/nuked/nukedopl3.c
	chips/nuked_opl3.cpp
	chips/nuked_opl3_v174.cpp
	wopl/wopl_file.c
)

add_library( adl STATIC ${ADL_SRCS} )

# ==================================== XP Patch - START ============================================
if (MadeForWinXP AND MSVC)
	message(STATUS "[adl] MadeForWinXP preset is ENABLED. Forcing clean compatibility flags pass...")
	
	foreach(source_file ${ADL_SRCS})
		# Force clear precompiled header flags and set explicit Windows XP targeting (0x0501)
		set_source_files_properties(${source_file} PROPERTIES 
			COMPILE_FLAGS "/Y-" 
			COMPILE_DEFINITIONS "WIN32_LEAN_AND_MEAN;_WIN32_WINNT=0x0501;WINVER=0x0501;IKNOW_UNKNOWN"
		)
	endforeach()
endif()
# ==================================== XP Patch - FINISH ===========================================

target_link_libraries( adl )
