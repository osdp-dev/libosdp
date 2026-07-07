#
#  Copyright (c) 2020-2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
#
#  SPDX-License-Identifier: Apache-2.0
#

set(GIT_REV "")
set(GIT_DIFF "")
set(GIT_TAG "")
set(GIT_BRANCH "None")

execute_process(
	COMMAND git -C ${CMAKE_SOURCE_DIR} rev-parse --is-inside-work-tree
	RESULT_VARIABLE _git_ok
	OUTPUT_QUIET
	ERROR_QUIET
)

if(_git_ok EQUAL 0)
	execute_process(
		COMMAND git -C ${CMAKE_SOURCE_DIR} describe --tags --long --always --abbrev=7
		OUTPUT_VARIABLE GIT_REV
		ERROR_QUIET
		OUTPUT_STRIP_TRAILING_WHITESPACE
	)

	execute_process(
		COMMAND git -C ${CMAKE_SOURCE_DIR} describe --exact-match --tags HEAD
		RESULT_VARIABLE _tag_rc
		OUTPUT_VARIABLE GIT_TAG
		ERROR_QUIET
		OUTPUT_STRIP_TRAILING_WHITESPACE
	)
	if(NOT _tag_rc EQUAL 0)
		set(GIT_TAG "")
	endif()

	execute_process(
		COMMAND git -C ${CMAKE_SOURCE_DIR} symbolic-ref --short -q HEAD
		RESULT_VARIABLE _branch_rc
		OUTPUT_VARIABLE GIT_BRANCH
		ERROR_QUIET
		OUTPUT_STRIP_TRAILING_WHITESPACE
	)
	if(NOT _branch_rc EQUAL 0 OR "${GIT_BRANCH}" STREQUAL "")
		set(GIT_BRANCH "detached")
	endif()

	execute_process(
		COMMAND git -C ${CMAKE_SOURCE_DIR} status --porcelain --untracked-files=normal
		OUTPUT_VARIABLE _git_status
		ERROR_QUIET
		OUTPUT_STRIP_TRAILING_WHITESPACE
	)
	if(NOT "${_git_status}" STREQUAL "")
		set(GIT_DIFF "+")
		if(NOT "${GIT_TAG}" STREQUAL "")
			set(GIT_TAG "${GIT_TAG}+")
		endif()
	endif()
endif()
