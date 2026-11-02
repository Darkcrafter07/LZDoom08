# ====================================================================================================
# CMakeLists.txt timidity
# ====================================================================================================
cmake_minimum_required( VERSION 3.16.0 )

use_fast_math()
require_stricmp(timidity PRIVATE)

if(MSVC)
	set(CMAKE_CXX_STANDARD 11)
	set(CMAKE_CXX_STANDARD_REQUIRED ON)
elseif( ZD_CMAKE_COMPILER_IS_GNUC_COMPATIBLE )
	set( CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11 -Wno-unused-function -Wno-unused-variable" )
endif()

include_directories( timidity )

file( GLOB HEADER_FILES
	timidity/*.h
)

# Collect all source files explicitly to apply properties dynamically
set( TIMIDITY_SRCS
	common.cpp
	instrum.cpp
	instrum_dls.cpp
	instrum_font.cpp
	instrum_sf2.cpp
	mix.cpp
	playmidi.cpp
	resample.cpp
	timidity.cpp
)

add_library( timidity STATIC
	${HEADER_FILES}
	${TIMIDITY_SRCS}
)

# ==== XP Patch: Force Windows XP environment targeting and completely drop PCH for Timidity ====
if(MadeForWinXP AND MSVC)
	message(STATUS "[timidity] MadeForWinXP preset is ENABLED. Forcing clean compatibility flags pass...")
	foreach(source_file ${TIMIDITY_SRCS})
		set_source_files_properties(${source_file} PROPERTIES
			COMPILE_FLAGS "/Y-"
			COMPILE_DEFINITIONS "WIN32_LEAN_AND_MEAN;_WIN32_WINNT=0x0501;WINVER=0x0501;IKNOW_UNKNOWN"
		)
	endforeach()
endif()
# ===============================================================================================

target_link_libraries( timidity )


