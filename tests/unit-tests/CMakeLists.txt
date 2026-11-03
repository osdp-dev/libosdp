#
#  Copyright (c) 2020-2026 Siddharth Chandrasekaran <sidcha@osdp.dev>
#
#  SPDX-License-Identifier: Apache-2.0
#

set(OSDP_UNIT_TEST osdp_unit_test)

# tests can include private headers
include_directories(${LIB_OSDP_INCLUDE_DIRS})
include_directories(${LIB_OSDP_PRIVATE_INCLUDE_DIRS})

# rebuild libosdp with test flag. Shorten the PD online timeout so
# link-loss tests do not block for the production 8s window, and the SC
# retry wait so a failed handshake is retried within a test's budget
# instead of after the production 600s, and the TRS idle timeout so an
# abandoned session closes within one instead of after 30s. The command retry
# wait is shortened so a case exhausting the retry budget costs well under a
# second instead of eight times 800ms.
set(LIB_OSDP_TEST osdptest)
add_definitions(-DUNIT_TESTING -DOSDP_PD_ONLINE_TOUT_MS=1500
		-DOSDP_PD_SC_RETRY_MS=3000 -DOSDP_TRS_IDLE_TOUT_MS=4000
		-DOSDP_CMD_RETRY_WAIT_MS=100
		${LIB_OSDP_DEFINITIONS})
add_library(${LIB_OSDP_TEST} STATIC EXCLUDE_FROM_ALL
	    ${LIB_OSDP_SOURCES} ${PROJECT_SOURCE_DIR}/src/osdp_file.c)

# Suite source files come from the shared manifest so the lean and CMake
# builds never drift; see test-sources.list.
file(STRINGS test-sources.list OSDP_UNIT_TEST_SRC REGEX "\\.c$")

add_executable(${OSDP_UNIT_TEST} EXCLUDE_FROM_ALL ${OSDP_UNIT_TEST_SRC})

target_link_libraries(${OSDP_UNIT_TEST} ${LIB_OSDP_TEST} osdp utils pthread)

# Zero-copy lane. OPT_OSDP_RX_ZERO_COPY defaults OFF, so the suite above never
# compiles the zero-copy RX path (its mocks use the byte-wise .recv callback).
# Build a second lib + test binary with the flag forced on, even in the default
# CMake config. The flag changes only how phy takes a frame off the channel and
# releases it, so the lane runs OSDP_ZC_SUITES: the suites whose CP<->PD
# exchanges reach every RX release path (each command and reply type, multipart
# legs, secure-channel frames, BUSY retransmits, rejected frames). Suites that
# re-drive protocol logic above phy, or never touch the channel, would only
# repeat the default lane. test-pd-zc.c adds zero-copy-specific coverage.
# osdptest_zc's PUBLIC define propagates the flag to
# the test binary's own sources; it links the zero-copy lib only (never the
# default-config `osdp` shared lib, which would pull in non-zero-copy objects).
set(LIB_OSDP_TEST_ZC osdptest_zc)
add_library(${LIB_OSDP_TEST_ZC} STATIC EXCLUDE_FROM_ALL ${LIB_OSDP_SOURCES})
target_compile_definitions(${LIB_OSDP_TEST_ZC} PUBLIC OPT_OSDP_RX_ZERO_COPY=1)

add_executable(${OSDP_UNIT_TEST}_zc EXCLUDE_FROM_ALL ${OSDP_UNIT_TEST_SRC})
target_link_libraries(${OSDP_UNIT_TEST}_zc
	${LIB_OSDP_TEST_ZC} utils ${LIB_OSDP_LIBRARIES} pthread)

# Static-allocation lane. OPT_OSDP_STATIC carves every context and every PD's
# feature state out of fixed pools, which the CP and PD contexts the harness
# runs in one process must not share. OSDP_STATIC_SUITES are the suites that
# drive those per-PD pools end to end; some other suites set up a second CP or
# PD context, which static mode cannot hold alongside the harness pair.
set(OSDP_STATIC_SUITES file_tx piv bio)
set(LIB_OSDP_TEST_STATIC osdptest_static)
add_library(${LIB_OSDP_TEST_STATIC} STATIC EXCLUDE_FROM_ALL ${LIB_OSDP_SOURCES})
target_compile_definitions(${LIB_OSDP_TEST_STATIC} PUBLIC OPT_OSDP_STATIC=1)

add_executable(${OSDP_UNIT_TEST}_static EXCLUDE_FROM_ALL ${OSDP_UNIT_TEST_SRC})
target_link_libraries(${OSDP_UNIT_TEST}_static
	${LIB_OSDP_TEST_STATIC} utils ${LIB_OSDP_LIBRARIES} pthread)

if (OPT_BUILD_SANITIZER)
	foreach(tgt ${OSDP_UNIT_TEST} ${OSDP_UNIT_TEST}_zc ${LIB_OSDP_TEST_ZC}
		    ${OSDP_UNIT_TEST}_static ${LIB_OSDP_TEST_STATIC})
		target_compile_options(${tgt} PRIVATE
			-fsanitize=address,undefined,leak
		)
		target_link_options(${tgt} PRIVATE
			-fsanitize=address,undefined,leak
		)
	endforeach()
endif()

include_directories(
	${PROJECT_SOURCE_DIR}/include
	${PROJECT_SOURCE_DIR}/utils/include
)

# CTest integration: one CTest test per suite so `ctest -R ut_piv` runs a
# single group and `ctest -jN` fans suites out as isolated processes. Process
# isolation is what makes the parallelism safe -- the harness keeps global mock
# state (one circbuf/pktq pair, one runner table) that is fine per-process but must
# never be shared across suites in threads.
#
# The suite name list is derived from test-suites.def -- the single source of
# truth the C dispatch table also uses -- so CTest registration cannot drift
# from the binary. SUITE(...) suites exist in both binaries; SUITE_ZC(...)
# suites are compiled only into the zero-copy binary.
file(STRINGS test-suites.def _osdp_suite_lines REGEX "^SUITE(_ZC)?\\(")
set(OSDP_SUITES "")
set(OSDP_SUITES_ZC_ONLY "")
foreach(_line ${_osdp_suite_lines})
	if(_line MATCHES "^SUITE_ZC\\(([A-Za-z0-9_]+)")
		list(APPEND OSDP_SUITES_ZC_ONLY ${CMAKE_MATCH_1})
	elseif(_line MATCHES "^SUITE\\(([A-Za-z0-9_]+)")
		list(APPEND OSDP_SUITES ${CMAKE_MATCH_1})
	endif()
endforeach()

set(OSDP_ZC_SUITES ${OSDP_SUITES_ZC_ONLY} abi commands events busy completion
		   file_tx piv sc_handshake)
foreach(_suite ${OSDP_ZC_SUITES})
	if(NOT _suite IN_LIST OSDP_SUITES AND
	   NOT _suite IN_LIST OSDP_SUITES_ZC_ONLY)
		message(FATAL_ERROR "OSDP_ZC_SUITES: unknown suite '${_suite}'")
	endif()
endforeach()

# Both binaries are EXCLUDE_FROM_ALL, so a plain `cmake --build` does not
# produce them. A fixture builds each on demand when ctest runs.
add_test(NAME osdp_unit_test_build
	COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR}
		--target ${OSDP_UNIT_TEST} --config $<CONFIG>
)
set_tests_properties(osdp_unit_test_build PROPERTIES
	FIXTURES_SETUP osdp_unit_test_built
)
add_test(NAME osdp_unit_test_zc_build
	COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR}
		--target ${OSDP_UNIT_TEST}_zc --config $<CONFIG>
)
set_tests_properties(osdp_unit_test_zc_build PROPERTIES
	FIXTURES_SETUP osdp_unit_test_zc_built
)
add_test(NAME osdp_unit_test_static_build
	COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR}
		--target ${OSDP_UNIT_TEST}_static --config $<CONFIG>
)
set_tests_properties(osdp_unit_test_static_build PROPERTIES
	FIXTURES_SETUP osdp_unit_test_static_built
)

# Suites that assert real-time protocol timing (file-transfer completion
# windows, retransmit/link-loss budgets) are unreliable under heavy CPU
# contention: mark them RUN_SERIAL so `ctest -jN` runs them alone and cannot
# starve their wall-clock wait loops. Keep this list minimal -- RUN_SERIAL on a
# long suite would erase the parallel speedup.
set(OSDP_SERIAL_SUITES file_tx)

# Each suite writes per-case JUnit XML next to the build, for CI reporting.
set(OSDP_JUNIT_DIR ${CMAKE_BINARY_DIR}/junit)
file(MAKE_DIRECTORY ${OSDP_JUNIT_DIR})

function(osdp_add_suite_tests prefix binary fixture)
	foreach(_suite ${ARGN})
		add_test(NAME ${prefix}${_suite}
			COMMAND ${binary} ${_suite}
				--junit ${OSDP_JUNIT_DIR}/${prefix}${_suite}.xml
			WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/bin
		)
		set_tests_properties(${prefix}${_suite} PROPERTIES
			FIXTURES_REQUIRED ${fixture}
		)
		if(_suite IN_LIST OSDP_SERIAL_SUITES)
			set_tests_properties(${prefix}${_suite} PROPERTIES
				RUN_SERIAL TRUE)
		endif()
	endforeach()
endfunction()

osdp_add_suite_tests(ut_ ${OSDP_UNIT_TEST} osdp_unit_test_built
		     ${OSDP_SUITES})
osdp_add_suite_tests(ut_zc_ ${OSDP_UNIT_TEST}_zc osdp_unit_test_zc_built
		     ${OSDP_ZC_SUITES})
osdp_add_suite_tests(ut_static_ ${OSDP_UNIT_TEST}_static
		     osdp_unit_test_static_built ${OSDP_STATIC_SUITES})

# `check` / `check-ut` stay as the single convenience wrapper around ctest.
cmake_host_system_information(RESULT _osdp_ncpu QUERY NUMBER_OF_LOGICAL_CORES)
add_custom_target(check
	COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure -j ${_osdp_ncpu}
		-R "^ut_"
	DEPENDS ${OSDP_UNIT_TEST} ${OSDP_UNIT_TEST}_zc ${OSDP_UNIT_TEST}_static
	USES_TERMINAL
)
add_custom_target(check-ut
	DEPENDS check
)
