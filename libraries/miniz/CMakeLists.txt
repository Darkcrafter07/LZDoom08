# CMakeLists.txt MINIZ
cmake_minimum_required( VERSION 3.16 )

make_release_only()

if (MSVC)
	set( CMAKE_C_FLAGS "${CMAKE_C_FLAGS} /wd4244" )
endif()

# Force library type to STATIC so it compiles into a solid miniz.lib instead of a DLL
add_library( miniz STATIC miniz.c )

# ==== XP Patch: Force Windows XP environment targeting and completely drop PCH for miniz ====
if(MadeForWinXP AND MSVC)
	message(STATUS "[miniz] MadeForWinXP preset is ENABLED. Forcing clean compatibility flags pass...")
	set_source_files_properties(miniz.c PROPERTIES 
		COMPILE_FLAGS "/Y-" 
		COMPILE_DEFINITIONS "WIN32_LEAN_AND_MEAN;_WIN32_WINNT=0x0501;WINVER=0x0501;IKNOW_UNKNOWN"
	)
endif()
# ============================================================================================

target_compile_definitions( miniz PRIVATE -DMINIZ_NO_STDIO )
target_include_directories( miniz INTERFACE "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>" )
