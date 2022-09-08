cmake_minimum_required(VERSION 3.17)
project(RipAndTear LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION OFF)
set(CMAKE_INSTALL_PREFIX "${CMAKE_SOURCE_DIR}/publish")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_MODULE_PATH ${CMAKE_BINARY_DIR} ${CMAKE_MODULE_PATH})
set(CMAKE_PREFIX_PATH ${CMAKE_BINARY_DIR} ${CMAKE_PREFIX_PATH})
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

# --------------------------------------------------
# Add formatting target
include(cmake/clang-format.cmake)
setup_format_target(format_rip_and_tear)

# --------------------------------------------------
# Get version tag
include(cmake/git-version.cmake)
get_git_version(RIP_AND_TEAR_GIT_VERSION)

# --------------------------------------------------
# Run conan
include(cmake/conan.cmake)
run_conan()

# --------------------------------------------------
# Find packages
find_package(structopt CONFIG REQUIRED)
find_package(magic_enum CONFIG REQUIRED)
find_package(fmt CONFIG REQUIRED)

# --------------------------------------------------
# Create interface libs
add_library(rip_and_tear_warnings INTERFACE)

if(MSVC)
	target_compile_options(rip_and_tear_warnings INTERFACE /W4 /WX /permissive-)
else()
	target_compile_options(rip_and_tear_warnings INTERFACE -Wall -Wextra -pedantic -Werror)
endif()

add_library(rip_and_tear_definitions INTERFACE)
target_compile_definitions(rip_and_tear_definitions INTERFACE
	_SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING
	WIN32_LEAN_AND_MEAN
	NOMINMAX)

option(RIP_AND_TEAR_UNITY_BUILD "Build batch sources for faster builds." OFF)

if(RIP_AND_TEAR_UNITY_BUILD)
	set_property(
		TARGET rip_and_tear_definitions
		PROPERTY UNITY_BUILD ON)
endif()

add_library(rip_and_tear_version STATIC
	"source/version/version.h" "source/version/version.cpp")
target_include_directories(rip_and_tear_version PUBLIC source/version)
target_compile_definitions(rip_and_tear_version PRIVATE
	RIP_AND_TEAR_VERSION=${RIP_AND_TEAR_GIT_VERSION})

add_library(rip_and_tear_dependencies INTERFACE)
target_link_libraries(rip_and_tear_dependencies INTERFACE
	structopt::structopt
	magic_enum::magic_enum
	fmt::fmt)

add_library(rip_and_tear_pch INTERFACE)
target_precompile_headers(rip_and_tear_pch INTERFACE
	<magic_enum.hpp>
	<fmt/format.h>
	<string>
	<string_view>
	<array>
	<vector>
	<deque>
	<map>
	<unordered_map>
	<set>
	<unordered_set>
	<fstream>
	<regex>
	<filesystem>
	<variant>
	<functional>
	<algorithm>
	<codecvt>
	<iomanip>
	<locale>
	<mutex>)

# --------------------------------------------------
# Create rip_and_tear executbale
file(GLOB_RECURSE rip_and_tear_sources CONFIGURE_DEPENDS "source/rip_and_tear/*.cpp")
file(GLOB_RECURSE rip_and_tear_headers CONFIGURE_DEPENDS "source/rip_and_tear/*.h" "source/rip_and_tear/*.inl")
set(rip_and_tear_resources "res/rip_and_tear.rc")
add_executable(rip_and_tear ${rip_and_tear_sources} ${shared_sources} ${rip_and_tear_headers} ${shared_headers} ${rip_and_tear_resources})
target_link_libraries(rip_and_tear PRIVATE
	rip_and_tear_warnings
	rip_and_tear_definitions
	rip_and_tear_dependencies
	rip_and_tear_pch)
target_include_directories(rip_and_tear PRIVATE "source/rip_and_tear" "res")

# --------------------------------------------------
# Merge files from source and include in the IDE
function(group_files sources)
	foreach(FILE ${sources})
		# Get the directory of the source file
		get_filename_component(PARENT_DIR "${FILE}" DIRECTORY)

		# Remove common directory prefix to make the group
		string(REPLACE "${CMAKE_CURRENT_SOURCE_DIR}" "" GROUP "${PARENT_DIR}")

		# Make sure we are using windows slashes
		string(REPLACE "/" "\\" GROUP "${GROUP}")

		# Strip the root parts for each possible component
		if("${FILE}" MATCHES "source/rip_and_tear/.*")
			string(SUBSTRING ${GROUP} 20 -1 GROUP)
		endif()

		# Do the grouping
		source_group("${GROUP}" FILES "${FILE}")
	endforeach()
endfunction()

group_files("${rip_and_tear_sources};${rip_and_tear_headers};${rip_and_tear_resources}")

# --------------------------------------------------
# Find the Spel2.exe, if not passed to cmake and set it for debugging in MSVC
if(NOT EXISTS ${SPELUNKY_INSTALL_DIR}/Spel2.exe)
	get_filename_component(STEAM_INSTALL_DIR "[HKEY_LOCAL_MACHINE\\SOFTWARE\\Wow6432Node\\Valve\\Steam;InstallPath]" ABSOLUTE)
	set(SPELUNKY_INSTALL_DIR "${STEAM_INSTALL_DIR}/SteamApps/common/Spelunky 2")

	if(NOT EXISTS ${SPELUNKY_INSTALL_DIR}/Spel2.exe)
		set(STEAM_LIBRARY_FOLDERS_CONFIG "${STEAM_INSTALL_DIR}/SteamApps/libraryfolders.vdf")

		if(EXISTS ${STEAM_LIBRARY_FOLDERS_CONFIG})
			FILE(READ "${STEAM_LIBRARY_FOLDERS_CONFIG}" STEAM_LIBRARY_FOLDERS_CONFIG_CONTENT)
			STRING(REGEX MATCHALL "\"[A-Z]\:[^\"]*\"" POTENTIAL_STEAM_LIBRARY_FOLDERS ${STEAM_LIBRARY_FOLDERS_CONFIG_CONTENT})

			foreach(STEAM_LIBRARY_FOLDER ${POTENTIAL_STEAM_LIBRARY_FOLDERS})
				string(REGEX REPLACE "\^\"" "" STEAM_LIBRARY_FOLDER ${STEAM_LIBRARY_FOLDER})
				string(REGEX REPLACE "\"$" "" STEAM_LIBRARY_FOLDER ${STEAM_LIBRARY_FOLDER})
				string(REGEX REPLACE "\\\\\\\\" "/" STEAM_LIBRARY_FOLDER ${STEAM_LIBRARY_FOLDER}) # double slash, escaped for cmake string then escaped for regex, requires a total of 8 backslashes

				if(EXISTS ${STEAM_LIBRARY_FOLDER})
					set(SPELUNKY_INSTALL_DIR "${STEAM_LIBRARY_FOLDER}/SteamApps/common/Spelunky 2")

					if(EXISTS ${SPELUNKY_INSTALL_DIR}/Spel2.exe)
						break()
					endif()
				endif()
			endforeach()
		endif()
	endif()

	if(NOT EXISTS ${SPELUNKY_INSTALL_DIR})
		message(STATUS "Could not find Spelunky 2 installation, pass it to cmake via -DSPELUNKY_INSTALL_DIR='Path/To/Install/Folder' or place the Spelunky 2 folder into the publish folder")
	else()
		message(STATUS "Found Spelunky 2 installation at '${SPELUNKY_INSTALL_DIR}'")
	endif()
endif()

# --------------------------------------------------
# Set debugging properties
if(EXISTS ${SPELUNKY_INSTALL_DIR})
	set_target_properties(rip_and_tear PROPERTIES
		VS_DEBUGGER_COMMAND_ARGUMENTS "--verbose --exe_dir \"${SPELUNKY_INSTALL_DIR}\"")
else()
	set_target_properties(rip_and_tear PROPERTIES
		VS_DEBUGGER_COMMAND_ARGUMENTS "--verbose")
endif()

set_target_properties(rip_and_tear PROPERTIES
	VS_DEBUGGER_WORKING_DIRECTORY $<TARGET_FILE_DIR:rip_and_tear>)
set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT rip_and_tear)

# --------------------------------------------------
# Install shared lib and launcher
install(TARGETS
	rip_and_tear
	RUNTIME
	DESTINATION .)

if(EXISTS ${SPELUNKY_INSTALL_DIR})
	install(FILES
		DESTINATION ${SPELUNKY_INSTALL_DIR})
endif()
