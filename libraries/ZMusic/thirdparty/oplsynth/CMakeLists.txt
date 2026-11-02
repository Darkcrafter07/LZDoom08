# ====================================================================================================
# CMakeLists.txt OPL synth
# ====================================================================================================
cmake_minimum_required( VERSION 3.16.0 )

use_fast_math()
require_stricmp(oplsynth PRIVATE)
require_strnicmp(oplsynth PRIVATE)

if(MSVC)
	set(CMAKE_CXX_STANDARD 11)
	set(CMAKE_CXX_STANDARD_REQUIRED ON)
elseif( ZD_CMAKE_COMPILER_IS_GNUC_COMPATIBLE )
	set( CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -Wno-unused-parameter -fomit-frame-pointer" )
	set( CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11" )
endif()

include_directories( oplsynth )

file( GLOB HEADER_FILES
	oplsynth/*.h
)

# Collect all source files explicitly to apply properties dynamically
set( OPLSYNTH_SRCS
	fmopl.cpp
	musicblock.cpp
	nukedopl3.cpp
	opl_mus_player.cpp
	OPL3.cpp
	oplio.cpp
	dosbox/opl.cpp
)

add_library( oplsynth STATIC
	${HEADER_FILES}
	${OPLSYNTH_SRCS}
)

# ==== XP Patch: Force Windows XP environment targeting and completely drop PCH for OPL sources ====
if(MadeForWinXP AND MSVC)
	message(STATUS "[oplsynth] MadeForWinXP preset is ENABLED. Forcing clean compatibility flags pass...")
	foreach(source_file ${OPLSYNTH_SRCS})
		set_source_files_properties(${source_file} PROPERTIES
			COMPILE_FLAGS "/Y-"
			COMPILE_DEFINITIONS "WIN32_LEAN_AND_MEAN;_WIN32_WINNT=0x0501;WINVER=0x0501;IKNOW_UNKNOWN"
		)
	endforeach()
endif()
# ===================================================================================================

target_link_libraries( oplsynth )


