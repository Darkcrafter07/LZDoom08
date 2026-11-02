# ====================================================================================================
# CMakeLists.txt wildmidi
# ====================================================================================================
cmake_minimum_required( VERSION 3.16.0 )

use_fast_math()
require_stricmp(wildmidi PRIVATE)
require_strnicmp(wildmidi PRIVATE)

if(MSVC)
	set(CMAKE_CXX_STANDARD 11)
	set(CMAKE_CXX_STANDARD_REQUIRED ON)
elseif( ZD_CMAKE_COMPILER_IS_GNUC_COMPATIBLE )
	set( CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -Wno-unused-parameter -fomit-frame-pointer" )
	set( CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11" )
endif()

include_directories( wildmidi )

file( GLOB HEADER_FILES
	wildmidi/*.h
)

# Collect all source files explicitly to apply properties dynamically
set( WILDMIDI_SRCS
	file_io.cpp
	gus_pat.cpp
	reverb.cpp
	wildmidi_lib.cpp
	wm_error.cpp
)

add_library( wildmidi STATIC
	${HEADER_FILES}
	${WILDMIDI_SRCS}
)

# ==== XP Patch: Force Windows XP environment targeting and completely drop PCH for WildMidi ====
if(MadeForWinXP AND MSVC)
	message(STATUS "[wildmidi] MadeForWinXP preset is ENABLED. Forcing clean compatibility flags pass...")
	foreach(source_file ${WILDMIDI_SRCS})
		set_source_files_properties(${source_file} PROPERTIES
			COMPILE_FLAGS "/Y-"
			COMPILE_DEFINITIONS "WIN32_LEAN_AND_MEAN;_WIN32_WINNT=0x0501;WINVER=0x0501;IKNOW_UNKNOWN"
		)
	endforeach()
endif()
# ===============================================================================================

target_link_libraries( wildmidi )


