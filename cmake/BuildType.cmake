#
#  Copyright (c) 2020-2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
#
#  SPDX-License-Identifier: Apache-2.0
#

set(BUILD_TYPE "Release")
if(EXISTS "${CMAKE_SOURCE_DIR}/.git")
	set(BUILD_TYPE "Debug")
endif()

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
	message(STATUS "Build type unspecified, setting to '${BUILD_TYPE}'")
	set(CMAKE_BUILD_TYPE "${BUILD_TYPE}" CACHE STRING
	    "Choose the type of build" FORCE)

	# Set the possible values of build type for cmake-gui
	set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS "Debug" "Release"
		     "MinSizeRel" "RelWithDebInfo")
endif()
