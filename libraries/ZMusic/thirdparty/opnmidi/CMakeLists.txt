cmake_minimum_required( VERSION 3.16 )

make_release_only()
use_fast_math()

# we play with out own sequencer
add_definitions(-DOPNMIDI_DISABLE_MIDI_SEQUENCER)

# Disable OPNMIDI's experimental yet emulator (using of it has some issues and missing notes in playback)
add_definitions(-DOPNMIDI_DISABLE_GX_EMULATOR)

# Collect all source files into a variable explicitly
set( OPN_SRCS
	chips/gens_opn2.cpp
	chips/gens/Ym2612_Emu.cpp
	chips/mame/mame_ym2612fm.c
	chips/mame_opn2.cpp
	chips/nuked_opn2.cpp
	chips/nuked/ym3438.c
	opnmidi.cpp
	opnmidi_load.cpp
	opnmidi_midiplay.cpp
	opnmidi_opn2.cpp
	opnmidi_private.cpp
	wopn/wopn_file.c
)

add_library( opn STATIC ${OPN_SRCS} )

# ==================================== XP Patch - START ============================================
if (MadeForWinXP AND MSVC)
	message(STATUS "[opn] MadeForWinXP preset is ENABLED. Forcing clean compatibility flags pass...")
	
	foreach(source_file ${OPN_SRCS})
		# Force clear precompiled header flags and set explicit Windows XP targeting (0x0501)
		set_source_files_properties(${source_file} PROPERTIES 
			COMPILE_FLAGS "/Y-" 
			COMPILE_DEFINITIONS "WIN32_LEAN_AND_MEAN;_WIN32_WINNT=0x0501;WINVER=0x0501;IKNOW_UNKNOWN"
		)
	endforeach()
endif()
# ==================================== XP Patch - FINISH ===========================================

target_link_libraries( opn )
