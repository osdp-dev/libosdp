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
# instead of after the production 600s.
set(LIB_OSDP_TEST osdptest)
add_definitions(-DUNIT_TESTING -DOSDP_PD_ONLINE_TOUT_MS=1500
		-DOSDP_PD_SC_RETRY_MS=3000 ${LIB_OSDP_DEFINITIONS})
add_library(${LIB_OSDP_TEST} STATIC EXCLUDE_FROM_ALL
	    ${LIB_OSDP_SOURCES} ${PROJECT_SOURCE_DIR}/src/osdp_file.c)

# Suite source files come from the shared manifest so the lean and CMake
# builds never drift; see test-sources.list.
file(STRINGS test-sources.list OSDP_UNIT_TEST_SRC REGEX "\\.c$")

add_executable(${OSDP_UNIT_TEST} EXCLUDE_FROM_ALL ${OSDP_UNIT_TEST_SRC})

target_link_libraries(${OSDP_UNIT_TEST} ${LIB_OSDP_TEST} osdp utils pthread)

# Zero-copy lane. OPT_OSDP_RX_ZERO_COPY defaults OFF, so the suite above never
# compiles the zero-copy RX path (its mocks use the byte-wise .recv callback).
# Build a second lib + test binary with the flag forced on so the whole suite
# runs under zero-copy too, even in the default CMake config. The general suites
# drive real CP<->PD exchanges over the packet-queue mock channel (test.c); the
# byte-stream framer tests (cp_phy/pd_phy) self-skip there, and test-pd-zc.c adds
# zero-copy-specific coverage. osdptest_zc's PUBLIC define propagates the flag to
# the test binary's own sources; it links the zero-copy lib only (never the
# default-config `osdp` shared lib, which would pull in non-zero-copy objects).
set(LIB_OSDP_TEST_ZC osdptest_zc)
add_library(${LIB_OSDP_TEST_ZC} STATIC EXCLUDE_FROM_ALL ${LIB_OSDP_SOURCES})
target_compile_definitions(${LIB_OSDP_TEST_ZC} PUBLIC OPT_OSDP_RX_ZERO_COPY=1)

add_executable(${OSDP_UNIT_TEST}_zc EXCLUDE_FROM_ALL ${OSDP_UNIT_TEST_SRC})
target_link_libraries(${OSDP_UNIT_TEST}_zc
	${LIB_OSDP_TEST_ZC} utils ${LIB_OSDP_LIBRARIES} pthread)

if (OPT_BUILD_SANITIZER)
	foreach(tgt ${OSDP_UNIT_TEST} ${OSDP_UNIT_TEST}_zc ${LIB_OSDP_TEST_ZC})
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
# state (one circbuf/pktq pair, one workqueue) that is fine per-process but must
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

# Suites that assert real-time protocol timing (file-transfer completion
# windows, retransmit/link-loss budgets) are unreliable under heavy CPU
# contention: mark them RUN_SERIAL so `ctest -jN` runs them alone and cannot
# starve their wall-clock wait loops. Keep this list minimal -- RUN_SERIAL on a
# long suite would erase the parallel speedup.
set(OSDP_SERIAL_SUITES file_tx)

foreach(_suite ${OSDP_SUITES})
	add_test(NAME ut_${_suite}
		COMMAND ${OSDP_UNIT_TEST} ${_suite}
		WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/bin
	)
	set_tests_properties(ut_${_suite} PROPERTIES
		FIXTURES_REQUIRED osdp_unit_test_built
	)
	if(_suite IN_LIST OSDP_SERIAL_SUITES)
		set_tests_properties(ut_${_suite} PROPERTIES RUN_SERIAL TRUE)
	endif()
endforeach()

foreach(_suite ${OSDP_SUITES} ${OSDP_SUITES_ZC_ONLY})
	add_test(NAME ut_zc_${_suite}
		COMMAND ${OSDP_UNIT_TEST}_zc ${_suite}
		WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/bin
	)
	set_tests_properties(ut_zc_${_suite} PROPERTIES
		FIXTURES_REQUIRED osdp_unit_test_zc_built
	)
	if(_suite IN_LIST OSDP_SERIAL_SUITES)
		set_tests_properties(ut_zc_${_suite} PROPERTIES RUN_SERIAL TRUE)
	endif()
endforeach()

# `check` / `check-ut` stay as the single convenience wrapper around ctest.
add_custom_target(check
	COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure -R "^ut_"
	DEPENDS ${OSDP_UNIT_TEST} ${OSDP_UNIT_TEST}_zc
	USES_TERMINAL
)
add_custom_target(check-ut
	DEPENDS check
)
