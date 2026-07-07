#
#  Copyright (c) 2020-2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
#
#  SPDX-License-Identifier: Apache-2.0
#

set(OSDP_UNIT_TEST osdp_unit_test)

# tests can include private headers
include_directories(${LIB_OSDP_INCLUDE_DIRS})
include_directories(${LIB_OSDP_PRIVATE_INCLUDE_DIRS})

# rebuild libosdp with test flag. Shorten the PD online timeout so
# link-loss tests do not block for the production 8s window.
set(LIB_OSDP_TEST osdptest)
add_definitions(-DUNIT_TESTING -DOSDP_PD_ONLINE_TOUT_MS=1500
		${LIB_OSDP_DEFINITIONS})
add_library(${LIB_OSDP_TEST} STATIC EXCLUDE_FROM_ALL
	    ${LIB_OSDP_SOURCES} ${PROJECT_SOURCE_DIR}/src/osdp_file.c)

list(APPEND OSDP_UNIT_TEST_SRC
	test.c
	test-cp-phy.c
	test-pd-phy.c
	test-cp-fsm.c
	test-file.c
	test-commands.c
	test-events.c
	test-hotplug.c
	test-notifications.c
	test-async-fuzz.c
	test-codec-fuzz.c
	test-sc.c
	test-sc-sia-vectors.c
	test-pd-zc.c
)

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

# CTest integration: lets `ctest` (and IDEs) discover the unit tests.
# osdp_unit_test is EXCLUDE_FROM_ALL so a plain `cmake --build` does not
# produce the binary. A fixture builds it on demand when ctest runs.
add_test(NAME osdp_unit_test_build
	COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR}
		--target ${OSDP_UNIT_TEST} --config $<CONFIG>
)
set_tests_properties(osdp_unit_test_build PROPERTIES
	FIXTURES_SETUP osdp_unit_test_built
)
add_test(NAME osdp_unit_test
	COMMAND ${OSDP_UNIT_TEST}
	WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)
set_tests_properties(osdp_unit_test PROPERTIES
	FIXTURES_REQUIRED osdp_unit_test_built
)

# Same on-demand build + run wiring for the zero-copy regression binary.
add_test(NAME osdp_unit_test_zc_build
	COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR}
		--target ${OSDP_UNIT_TEST}_zc --config $<CONFIG>
)
set_tests_properties(osdp_unit_test_zc_build PROPERTIES
	FIXTURES_SETUP osdp_unit_test_zc_built
)
add_test(NAME osdp_unit_test_zc
	COMMAND ${OSDP_UNIT_TEST}_zc
	WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)
set_tests_properties(osdp_unit_test_zc PROPERTIES
	FIXTURES_REQUIRED osdp_unit_test_zc_built
)

# `check` and `check-ut` remain as convenience wrappers around ctest so
# existing docs and CI continue to work unchanged.
add_custom_target(check
	COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure -R osdp_unit_test
	DEPENDS ${OSDP_UNIT_TEST} ${OSDP_UNIT_TEST}_zc
	USES_TERMINAL
)
add_custom_target(check-ut
	DEPENDS check
)
