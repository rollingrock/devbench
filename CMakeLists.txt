cmake_minimum_required(VERSION 3.23)

# devbench — Fallout 4 / Fallout 4 VR build.
#
# WHY TWO BUILD SYSTEMS LIVE IN THIS REPO: the Skyrim plugin is built with xmake
# (xmake.lua) because CommonLibSSE-NG ships an xmake package and that is the
# ecosystem's convention; the Fallout plugin is built with CMake because CommonLibF4
# ships a CMake target and F4SE plugins are built that way. Neither game's toolchain
# is imposed on the other. The shared code in src/core/ is buildsystem-neutral by
# construction: it includes no script-extender header, so both builds compile the
# same sources with no per-buildsystem #ifdefs.

set(NAME "devbench")
set(VERSION 1.14.0)

project(${NAME} VERSION ${VERSION} LANGUAGES CXX)

# ---- Cache build vars ----
macro(set_from_environment VARIABLE)
	if(NOT DEFINED ${VARIABLE} AND DEFINED ENV{${VARIABLE}})
		set(${VARIABLE} $ENV{${VARIABLE}})
	endif()
endmacro()

set_from_environment(VCPKG_ROOT)
set_from_environment(CommonLibF4Path)
set_from_environment(FalloutVRPath)
set_from_environment(Fallout4Path)

if(NOT DEFINED CommonLibF4Path OR "${CommonLibF4Path}" STREQUAL "")
	find_path(CommonLibF4Path
		CommonLibF4/include/REL/Relocation.h
		PATHS extern/CommonLibF4 external/CommonLibF4)
endif()
if(NOT CommonLibF4Path OR "${CommonLibF4Path}" STREQUAL "CommonLibF4Path-NOTFOUND")
	message(FATAL_ERROR
		"CommonLibF4 not found. Set the CommonLibF4Path environment variable, or place a checkout at extern/CommonLibF4. "
		"The VR-capable fork is https://github.com/rollingrock/CommonLibF4 — the upstream (flat-rim-only) one builds but cannot load in Fallout 4 VR.")
endif()

if(PROJECT_SOURCE_DIR STREQUAL PROJECT_BINARY_DIR)
	message(FATAL_ERROR "In-source builds are not allowed; configure into a separate build directory.")
endif()

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>" CACHE STRING "")
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_DEBUG OFF)
set(Boost_USE_STATIC_LIBS ON)
set(Boost_USE_STATIC_RUNTIME OFF CACHE BOOL "")

add_compile_definitions(_CRT_SECURE_NO_WARNINGS)

# ---- Version header (shared with the xmake/Skyrim build) ----
set(VERSION_MAJOR ${PROJECT_VERSION_MAJOR})
set(VERSION_MINOR ${PROJECT_VERSION_MINOR})
set(VERSION_PATCH ${PROJECT_VERSION_PATCH})
set(VERSION_STRING ${PROJECT_VERSION})
configure_file(
	${CMAKE_CURRENT_SOURCE_DIR}/src/Version.h.in
	${CMAKE_CURRENT_BINARY_DIR}/include/Version.h
)

# ---- Dependencies ----
add_subdirectory(${CommonLibF4Path} CommonLibF4)
find_package(nlohmann_json CONFIG REQUIRED)
find_path(STB_INCLUDE_DIRS "stb_image.h" REQUIRED)
include(cmake/cpp-mcp.cmake)

# ---- Sources ----
# The core is compiled from source into the plugin rather than built as its own
# library target: it is header-light, there is exactly one consumer per build, and a
# separate target would need its own set of the plugin's compile definitions to stay
# ABI-identical. If a second consumer ever appears, promote it then.
file(GLOB_RECURSE core_sources CONFIGURE_DEPENDS "src/core/*.cpp" "src/core/*.h")
file(GLOB_RECURSE fallout_sources CONFIGURE_DEPENDS "src/platform/fallout4/*.cpp" "src/platform/fallout4/*.h")

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR}/src FILES ${core_sources} ${fallout_sources})

add_library(${PROJECT_NAME} SHARED
	${core_sources}
	${fallout_sources}
	${CMAKE_CURRENT_BINARY_DIR}/include/Version.h
)

target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_23)

target_compile_definitions(${PROJECT_NAME} PRIVATE
	_UNICODE
	UNICODE
	# cpp-mcp's vendored httplib pulls in <winsock2.h>; CommonLibF4's transitive
	# <Windows.h> would otherwise include the legacy <winsock.h> and conflict.
	_WINSOCKAPI_
	F4SE_SUPPORT_XBYAK
)

target_include_directories(${PROJECT_NAME} PRIVATE
	${CMAKE_CURRENT_BINARY_DIR}/include
	${CMAKE_CURRENT_SOURCE_DIR}/src
	${CMAKE_CURRENT_SOURCE_DIR}/src/platform/fallout4
	${STB_INCLUDE_DIRS}
)

target_link_libraries(${PROJECT_NAME} PRIVATE
	CommonLibF4::CommonLibF4
	nlohmann_json::nlohmann_json
	cpp-mcp
)

target_precompile_headers(${PROJECT_NAME} PRIVATE src/platform/fallout4/pch.h)

if(MSVC)
	target_compile_options(${PROJECT_NAME} PRIVATE
		/utf-8
		/Zi
		/permissive-
		/Zc:__cplusplus
		/Zc:preprocessor
		/Zc:inline
		/EHsc
		/W4
		/external:anglebrackets
		/external:W0
		# The MSVC-specific narrowing/unused warnings CommonLib-adjacent code trips over.
		/wd4189 /wd4244 /wd4267
		"$<$<CONFIG:RELEASE>:/Ob3>"
	)
	target_link_options(${PROJECT_NAME} PRIVATE
		"$<$<CONFIG:DEBUG>:/INCREMENTAL;/OPT:NOREF;/OPT:NOICF>"
		"$<$<CONFIG:RELEASE>:/INCREMENTAL:NO;/OPT:REF;/OPT:ICF;/DEBUG:FULL>"
	)
	string(REPLACE "/Ob2" "/Ob3" CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE}")
endif()

# ---- Deploy ----
# Set FalloutPluginTargets to one or more game Data dirs separated by ';' — the CMake
# twin of the Skyrim build's SkyrimPluginTargets, so the same muscle memory works.
set_from_environment(FalloutPluginTargets)
if(DEFINED FalloutPluginTargets AND NOT "${FalloutPluginTargets}" STREQUAL "")
	foreach(_dir ${FalloutPluginTargets})
		add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
			COMMAND ${CMAKE_COMMAND} -E make_directory "${_dir}/F4SE/Plugins"
			COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:${PROJECT_NAME}> "${_dir}/F4SE/Plugins/"
			COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_PDB_FILE:${PROJECT_NAME}> "${_dir}/F4SE/Plugins/"
		)
	endforeach()
endif()
