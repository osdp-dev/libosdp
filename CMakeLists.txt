#
#  Copyright (c) 2019-2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
#
#  SPDX-License-Identifier: Apache-2.0
#

cmake_minimum_required(VERSION 3.14 FATAL_ERROR)
cmake_policy(SET CMP0063 NEW)

# Generate compile_commands.json for IDE support
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

project(libosdp VERSION 4.0.0)

# Pre-release marker: "dev" while the next release is prepared but unpublished,
# empty once published. Kept separate from the numeric project() version (which
# SOVERSION/CPack/pkg-config derive from) — it only decorates the human-visible
# version string composed in cmake/GitInfo.cmake. Owned by scripts/make_release.py.
set(LIBOSDP_PRERELEASE "dev")

set(PROJECT_AUTHOR       "Siddharth Chandrasekaran")
set(PROJECT_AUTHOR_EMAIL "sidcha.dev@gmail.com")
set(PROJECT_YEAR         "2019")
set(PROJECT_ORG          "osdp-dev")
set(PROJECT_URL          "https://github.com/osdp-dev/libosdp/")
set(PROJECT_HOMEPAGE     "https://doc.osdp.dev/")
set(PROJECT_DESCRIPTION  "Open Supervised Device Protocol (OSDP) Library")
set(PROJECT_LICENSE      "Apache License, Version 2.0 (Apache-2.0)")

## Options
option(OPT_OSDP_PACKET_TRACE "Enable raw packet trace for diagnostics" OFF)
option(OPT_OSDP_DATA_TRACE "Enable command/reply data buffer tracing" OFF)
option(OPT_OSDP_SKIP_MARK_BYTE "Don't send the leading mark byte (0xFF)" OFF)
option(OPT_OSDP_RX_ZERO_COPY "Enable zero-copy RX buffers (requires recv_pkt/release_pkt)" OFF)
option(OPT_OSDP_LOG_MINIMAL "Minimize logger RAM/stack usage for embedded targets" OFF)
option(OPT_DISABLE_PRETTY_LOGGING "Don't colorize log ouputs" OFF)
option(OPT_BUILD_SANITIZER "Enable different sanitizers during build" OFF)
option(OPT_BUILD_STATIC "Build static library" ON)
option(OPT_BUILD_SHARED "Build shared library" ON)
option(OPT_OSDP_STATIC "Build without dynamic memory allocation" OFF)
option(OPT_OSDP_LIB_ONLY "Only build the library" OFF)
option(OPT_BUILD_BARE_METAL "Build library for bare metal targets" OFF)
option(OPT_USE_32BIT_TICK_T "Use uint32_t tick_t on bare-metal targets" OFF)
option(OPT_BUILD_OSDP_TRS "Enable Transparent Reader Support" OFF)
set(OPT_OSDP_CRYPTO_BACKEND "auto" CACHE STRING
	"Crypto backend selection: auto, openssl, mbedtls, or tinyaes")
set_property(CACHE OPT_OSDP_CRYPTO_BACKEND PROPERTY STRINGS
	auto openssl mbedtls tinyaes)

## Includes
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(AddCCompilerFlag)
include(GitInfo)
include(BuildType)

## Global settings
if(NOT MSVC)
	add_compile_options(-Wall -Wextra)
else()
	# MSVC's legacy preprocessor mis-handles the token-paste + rescan used
	# by IS_ENABLED() in utils/include/utils/utils.h, silently returning 0
	# for every defined-as-1 flag. /Zc:preprocessor switches to the
	# C99/C11-conformant preprocessor (VS 2019 16.5+). /std:c11 is also
	# required so _Static_assert (used by the IS_ENABLED self-test in
	# utils/src/utils.c) is recognised as a keyword.
	add_compile_options(/Zc:preprocessor /std:c11)
endif()

if (OPT_BUILD_BARE_METAL)
	# __BARE_METAL__ enables bare-metal paths. To use uint32_t tick_t,
	# also enable OPT_USE_32BIT_TICK_T. Verify your tick source and counter width
	# can safely represent the longest delay/timeout in your target project.
	add_compile_options(-D__BARE_METAL__)
	if (OPT_USE_32BIT_TICK_T)
		add_compile_options(-DUSE_32BIT_TICK_T)
	endif()
else()
	include(GNUInstallDirs)
endif()

## Optional external tooling — informational only, never required.
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
	message(STATUS "Python bindings: build with 'pip install ./python' (interpreter ${Python3_EXECUTABLE})")
else()
	message(STATUS "Python bindings: skipped (no Python 3 interpreter found)")
endif()

find_program(DOXYGEN_EXECUTABLE doxygen)
if(DOXYGEN_EXECUTABLE)
	set(DOXYGEN_OUTPUT_DIR "${CMAKE_BINARY_DIR}/doxygen")
	configure_file(
		"${CMAKE_CURRENT_SOURCE_DIR}/cmake/Doxyfile.in"
		"${CMAKE_CURRENT_BINARY_DIR}/Doxyfile"
		@ONLY
	)
	add_custom_target(doxygen_xml
		COMMAND "${DOXYGEN_EXECUTABLE}" "${CMAKE_CURRENT_BINARY_DIR}/Doxyfile"
		WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
		COMMENT "Generating Doxygen XML"
		VERBATIM
	)
else()
	message(STATUS "Doxygen not found; doxygen_xml target unavailable")
endif()

set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_WARN_DEPRECATED ON)

# Each subdirectory has it's own CMakeLists.txt
include(GitSubmodules)
add_subdirectory(src)
if (NOT OPT_OSDP_STATIC AND NOT OPT_OSDP_LIB_ONLY AND NOT MSVC)
	enable_testing()
	add_subdirectory(utils)
	add_subdirectory(tests/unit-tests)
	add_subdirectory(examples/c)
	add_subdirectory(examples/cpp)
endif()

## uninstall target. Dedupe the manifest and tolerate already-removed
## entries so re-runs (or future install rule changes) stay green.
add_custom_target(uninstall
	COMMAND sort -u ${CMAKE_BINARY_DIR}/install_manifest.txt | xargs rm -f
)

## include package rules at last
include(CreatePackages)
