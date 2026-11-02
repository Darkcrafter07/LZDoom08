cmake_minimum_required( VERSION 3.16 )

use_fast_math()

if(MSVC)
	set(CMAKE_CXX_STANDARD 11)
	set(CMAKE_CXX_STANDARD_REQUIRED ON)
elseif( ZD_CMAKE_COMPILER_IS_GNUC_COMPATIBLE )
	set( CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11" )
endif()

include_directories( timiditypp )

file( GLOB HEADER_FILES
	timiditypp/*.h
)

# Collect all source files explicitly to apply properties dynamically
set( TIMIDITYPLUS_SRCS
	fft4g.cpp
	reverb.cpp
	common.cpp
	configfile.cpp
	effect.cpp
	filter.cpp
	freq.cpp
	instrum.cpp
	mblock.cpp
	mix.cpp
	playmidi.cpp
	quantity.cpp
	readmidic.cpp
	recache.cpp
	resample.cpp
	sbkconv.cpp
	sffile.cpp
	sfitem.cpp
	smplfile.cpp
	sndfont.cpp
	tables.cpp
)

add_library( timidityplus STATIC
	${HEADER_FILES}
	${TIMIDITYPLUS_SRCS}
)

# ==== XP Patch: Force Windows XP environment targeting and completely drop PCH for TimidityPlus ====
if(MadeForWinXP AND MSVC)
	message(STATUS "[timidityplus] MadeForWinXP preset is ENABLED. Forcing clean compatibility flags pass...")
	foreach(source_file ${TIMIDITYPLUS_SRCS})
		set_source_files_properties(${source_file} PROPERTIES
			COMPILE_FLAGS "/Y-"
			COMPILE_DEFINITIONS "WIN32_LEAN_AND_MEAN;_WIN32_WINNT=0x0501;WINVER=0x0501;IKNOW_UNKNOWN"
		)
	endforeach()
endif()
# ===================================================================================================

target_link_libraries( timidityplus )
