#CMakeLists.txt ROOT (ZMusic Modernized to 3.6.0+)
cmake_minimum_required( VERSION 3.16.0 )
project(ZMusic)

set(ZMUSIC_VERSION_MAJOR 1)
set(ZMUSIC_VERSION_MINOR 1)
set(ZMUSIC_VERSION_PATCH 0)
set(ZMUSIC_VERSION "${ZMUSIC_VERSION_MAJOR}.${ZMUSIC_VERSION_MINOR}.${ZMUSIC_VERSION_PATCH}")

# Ensure modern properties handling
if( POLICY CMP0054 )
	cmake_policy( SET CMP0054 NEW )
endif()

list( APPEND CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/cmake )
include( FindPackageHandleStandardArgs )

# Global configuration switches
option(MadeForWinXP     "Enable compatibility configurations for Windows XP" OFF)
option(IKnowTheUnknown  "Enable the IKnowTheUnknown preset configuration" OFF)

# Support cross compiling
option( FORCE_CROSSCOMPILE "Turn on cross compiling." NO )
if( FORCE_CROSSCOMPILE )
	set( CMAKE_CROSSCOMPILING TRUE )
endif()

if(CMAKE_CROSSCOMPILING)
	set(IMPORT_EXECUTABLES "IMPORTFILE-NOTFOUND" CACHE FILEPATH "Export file from native build.")
	include(${IMPORT_EXECUTABLES})
endif()

# Macro for building libraries without debugging information
macro( make_release_only )
	set( CMAKE_C_FLAGS_MINSIZEREL ${CMAKE_C_FLAGS_RELEASE} )
	set( CMAKE_C_FLAGS_RELWITHDEBINFO ${CMAKE_C_FLAGS_RELEASE} )
	string( REPLACE "/MT " "/MTd " CMAKE_C_FLAGS_DEBUG ${CMAKE_C_FLAGS_RELEASE} )
	set( CMAKE_CXX_FLAGS_MINSIZEREL ${CMAKE_CXX_FLAGS_RELEASE} )
	set( CMAKE_CXX_FLAGS_RELWITHDEBINFO ${CMAKE_CXX_FLAGS_RELEASE} )
	string( REPLACE "/MT " "/MTd " CMAKE_CXX_FLAGS_DEBUG ${CMAKE_CXX_FLAGS_RELEASE} )
endmacro()

if( NOT CMAKE_BUILD_TYPE )
	set( CMAKE_BUILD_TYPE Debug CACHE STRING
		"Choose the type of build, options are: Debug Release RelWithDebInfo MinSizeRel."
		FORCE )
endif()

# Compiler compatibility layout
if( CMAKE_C_COMPILER_ID MATCHES "GNU|Clang" )
	set( ZD_CMAKE_COMPILER_IS_GNUC_COMPATIBLE TRUE )
else()
	set( ZD_CMAKE_COMPILER_IS_GNUC_COMPATIBLE FALSE )
endif()

if( CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang" )
	set( ZD_CMAKE_COMPILER_IS_GNUCXX_COMPATIBLE TRUE )
else()
	set( ZD_CMAKE_COMPILER_IS_GNUCXX_COMPATIBLE FALSE )
endif()

if( ZD_CMAKE_COMPILER_IS_GNUCXX_COMPATIBLE )
	set( PROFILE 0 CACHE BOOL "Enable profiling with gprof for Debug and RelWithDebInfo build types." )
endif()

# Fast math flags, required by some subprojects
set( ZD_FASTMATH_FLAG "" )
if( ZD_CMAKE_COMPILER_IS_GNUCXX_COMPATIBLE )
	set( ZD_FASTMATH_FLAG "-ffast-math -ffp-contract=fast" )
elseif( MSVC )
	set( ZD_FASTMATH_FLAG "/fp:fast" )
endif()

macro( use_fast_math )
	set( CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${ZD_FASTMATH_FLAG}" )
	set( CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${ZD_FASTMATH_FLAG}" )
endmacro()

include( CheckFunctionExists )

macro( require_stricmp )
	CHECK_FUNCTION_EXISTS( stricmp STRICMP_EXISTS )
	if( NOT STRICMP_EXISTS )
		add_definitions( -Dstricmp=strcasecmp )
	endif()
endmacro()

macro( require_strnicmp )
	CHECK_FUNCTION_EXISTS( strnicmp STRNICMP_EXISTS )
	if( NOT STRNICMP_EXISTS )
		add_definitions( -Dstrnicmp=strncasecmp )
	endif()
endmacro()

find_package( ZLIB )

if( MSVC )
	# Optimize binaries layout
	set( REL_LINKER_FLAGS "/opt:ref /opt:icf /nodefaultlib:msvcrt /TSAWARE /LARGEADDRESSAWARE" )
	set( ALL_C_FLAGS "/GF /Gy /GR- /permissive-" )

	option( ZDOOM_GENERATE_ASM "Generate assembly output." OFF )
	if( ZDOOM_GENERATE_ASM )
		set( REL_C_FLAGS "/MT /Oy /Oi /FAcs /GS-" )
	else()
		set( REL_C_FLAGS "/MT /Oy /Oi /GS-" )
	endif()

	set( DEB_C_FLAGS "/D _CRTDBG_MAP_ALLOC /MTd" )

	# Target environment platform macro (Modern Windows 0x0600 vs Windows XP 0x0501)
	if(MadeForWinXP)
		set( ALL_C_FLAGS "${ALL_C_FLAGS} /wd4996 /DUNICODE /D_UNICODE /D_WIN32_WINNT=0x0501 /DXPPATCH" )
	else()
		set( ALL_C_FLAGS "${ALL_C_FLAGS} /wd4996 /DUNICODE /D_UNICODE /D_WIN32_WINNT=0x0600" )
	endif()

	# Inject preset definitions safely into compile options block
	if(IKnowTheUnknown)
		set( ALL_C_FLAGS "${ALL_C_FLAGS} /DIKNOW_UNKNOWN" )
	endif()

	string(REPLACE "/MD " " " CMAKE_CXX_FLAGS_RELEASE ${CMAKE_CXX_FLAGS_RELEASE} )
	string(REPLACE "/MD " " " CMAKE_CXX_FLAGS_MINSIZEREL ${CMAKE_CXX_FLAGS_MINSIZEREL} )
	string(REPLACE "/MD " " " CMAKE_CXX_FLAGS_RELWITHDEBINFO ${CMAKE_CXX_FLAGS_RELWITHDEBINFO} )
	string(REPLACE "/Ob1 " "/Ob2 " CMAKE_CXX_FLAGS_RELWITHDEBINFO ${CMAKE_CXX_FLAGS_RELWITHDEBINFO} )
	string(REPLACE "/MDd " " " CMAKE_CXX_FLAGS_DEBUG ${CMAKE_CXX_FLAGS_DEBUG} )
	string(REPLACE "/MD " " " CMAKE_C_FLAGS_RELEASE ${CMAKE_C_FLAGS_RELEASE} )
	string(REPLACE "/MD " " " CMAKE_C_FLAGS_MINSIZEREL ${CMAKE_C_FLAGS_MINSIZEREL} )
	string(REPLACE "/MD " " " CMAKE_C_FLAGS_RELWITHDEBINFO ${CMAKE_C_FLAGS_RELWITHDEBINFO} )
	string(REPLACE "/MDd " " " CMAKE_C_FLAGS_DEBUG ${CMAKE_C_FLAGS_DEBUG} )
	string(REPLACE " /GR" " " CMAKE_CXX_FLAGS ${CMAKE_CXX_FLAGS} )
else()
	set( REL_LINKER_FLAGS "" )
	set( ALL_C_FLAGS "-ffp-contract=off" )

	if(IKnowTheUnknown)
		set( ALL_C_FLAGS "${ALL_C_FLAGS} -DIKNOW_UNKNOWN" )
	endif()

	if ( UNIX )
		include(CheckSymbolExists)
		check_symbol_exists( "fts_set" "fts.h" HAVE_FTS )
		if ( NOT HAVE_FTS )
			include ( FindPkgConfig )
			pkg_check_modules( MUSL_FTS musl-fts )
			if ( MUSL_FTS_FOUND )
				set ( ALL_C_FLAGS "${ALL_C_FLAGS} ${MUSL_FTS_LDFLAGS}" )
			else ( MUSL_FTS_FOUND )
				message (ERROR "fts_* functions not found in the system" )
			endif ( MUSL_FTS_FOUND )
		endif ( NOT HAVE_FTS )
	endif ( UNIX )

	set( REL_C_FLAGS "" )
	set( DEB_C_FLAGS "" )

	if( APPLE )
		set( CMAKE_OSX_DEPLOYMENT_TARGET "10.9" )
		if( CMAKE_CXX_COMPILER_ID STREQUAL "Clang" )
			set( CMAKE_CXX_FLAGS "-stdlib=libc++ ${CMAKE_CXX_FLAGS}" )
			set( CMAKE_EXE_LINKER_FLAGS "-stdlib=libc++ ${CMAKE_EXE_LINKER_FLAGS}" )
		elseif( CMAKE_CXX_COMPILER_ID STREQUAL "GNU" )
			set( ALL_C_FLAGS "-static-libgcc" )
		endif()
	elseif( NOT MINGW )
		set( ALL_C_FLAGS "${ALL_C_FLAGS} -fPIC" )
	endif( APPLE )
endif()

set( CMAKE_EXE_LINKER_FLAGS_RELEASE "${CMAKE_EXE_LINKER_FLAGS_RELEASE} ${REL_LINKER_FLAGS}" )
set( CMAKE_EXE_LINKER_FLAGS_MINSIZEREL "${CMAKE_EXE_LINKER_FLAGS_MINSIZEREL} ${REL_LINKER_FLAGS}" )
set( CMAKE_EXE_LINKER_FLAGS_RELWITHDEBINFO "${CMAKE_EXE_LINKER_FLAGS_RELWITHDEBINFO} ${REL_LINKER_FLAGS}" )

set( CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${ALL_C_FLAGS}" )
set( CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} ${REL_C_FLAGS}" )
set( CMAKE_C_FLAGS_MINSIZEREL "${CMAKE_C_FLAGS_MINSIZEREL} ${REL_C_FLAGS}" )
set( CMAKE_C_FLAGS_RELWITHDEBINFO "${CMAKE_C_FLAGS_RELWITHDEBINFO} ${REL_C_FLAGS}" )
set( CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} ${DEB_C_FLAGS} -D_DEBUG" )

set( CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${ALL_C_FLAGS}" )
set( CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} ${REL_C_FLAGS}" )
set( CMAKE_CXX_FLAGS_MINSIZEREL "${CMAKE_CXX_FLAGS_MINSIZEREL} ${REL_C_FLAGS}" )
set( CMAKE_CXX_FLAGS_RELWITHDEBINFO "${CMAKE_CXX_FLAGS_RELWITHDEBINFO} ${REL_C_FLAGS}" )
set( CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} ${DEB_C_FLAGS} -D_DEBUG" )

option(FORCE_INTERNAL_ZLIB "Use internal zlib")
option(FORCE_INTERNAL_GME "Use internal gme" ON)
mark_as_advanced( FORCE_INTERNAL_GME )

if( ZLIB_FOUND AND NOT FORCE_INTERNAL_ZLIB )
	message( STATUS "Using system zlib, includes found at ${ZLIB_INCLUDE_DIR}" )
else()
	message( STATUS "Using internal zlib" )
	set( SKIP_INSTALL_ALL TRUE ) 
	add_subdirectory( thirdparty/zlib )
	set( ZLIB_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/zlib )
	set( ZLIB_LIBRARIES z )
	set( ZLIB_LIBRARY z )
endif()

if( GME_FOUND AND NOT FORCE_INTERNAL_GME )
	message( STATUS "Using system gme library, includes found at ${GME_INCLUDE_DIR}" )
else()
	message( STATUS "Using internal gme library" )
	set( GME_YM2612_EMU "MAME" )
	add_subdirectory( thirdparty/game-music-emu )
	set( GME_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/game-music-emu" )
	set( GME_LIBRARIES gme )
endif()

# ==================================== End of File Logic Section ===================================
set( ADL_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/adlmidi" )
set( OPN_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/opnmidi" )
set( TIMIDITYPP_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/timidityplus" )
set( TIMIDITY_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/timidity" )
set( WILDMIDI_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/wildmidi" )
set( OPLSYNTH_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/oplsynth" )

if( NOT CMAKE_CROSSCOMPILING )
	if( NOT CROSS_EXPORTS )
		set( CROSS_EXPORTS "" )
	endif()
endif()

option( DYN_FLUIDSYNTH "Dynamically load fluidsynth" ON )
option( DYN_SNDFILE "Dynamically load libsndfile" ON )
option( DYN_MPG123 "Dynamically load libmpg123" ON )

add_subdirectory( thirdparty/dumb )
add_subdirectory( thirdparty/adlmidi )
add_subdirectory( thirdparty/opnmidi )
add_subdirectory( thirdparty/timidity )
add_subdirectory( thirdparty/timidityplus )
add_subdirectory( thirdparty/wildmidi )
add_subdirectory( thirdparty/oplsynth )
add_subdirectory( source )

# ==== Automated Fix: Only export executables if ZMusic is built standalone ====
if( NOT CMAKE_CROSSCOMPILING AND PROJECT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR )
	export(TARGETS ${CROSS_EXPORTS} FILE "${CMAKE_BINARY_DIR}/ImportExecutables.cmake" )
endif()
# ==============================================================================


