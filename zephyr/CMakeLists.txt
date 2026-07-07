if (CONFIG_LIBOSDP)
	set(OSDP_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)
	set(REPO_ROOT ${OSDP_ROOT})

	list(APPEND CMAKE_MODULE_PATH "${OSDP_ROOT}/cmake")
	include(GitInfo)

	configure_file(${OSDP_ROOT}/src/osdp_config.h.in ${CMAKE_CURRENT_BINARY_DIR}/include/osdp_config.h @ONLY)

	file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/include/osdp_export.h "
		#ifndef OSDP_EXPORT_H_
		#define OSDP_EXPORT_H_

		#define OSDP_EXPORT
		#define OSDP_NO_EXPORT
		#define OSDP_DEPRECATED_EXPORT

		#endif /* OSDP_EXPORT_H_ */"
	)

	zephyr_interface_library_named(osdp)
	target_link_libraries(zephyr_interface INTERFACE osdp)
	target_include_directories(osdp INTERFACE ${OSDP_ROOT}/include)
	target_include_directories(osdp INTERFACE ${OSDP_ROOT}/zephyr/include)
	target_include_directories(osdp INTERFACE ${CMAKE_CURRENT_BINARY_DIR}/include)

	zephyr_library_named(modules_osdp)

	# Pass all OSDP Kconfig values as compile definitions
	zephyr_library_compile_definitions(OSDP_PACKET_BUF_SIZE=${CONFIG_OSDP_PACKET_BUF_SIZE})
	zephyr_library_compile_definitions(OSDP_MINIMUM_PACKET_SIZE=${CONFIG_OSDP_MINIMUM_PACKET_SIZE})
	zephyr_library_compile_definitions(OSDP_RX_RB_SIZE=${CONFIG_OSDP_RX_RB_SIZE})
	zephyr_library_compile_definitions(OSDP_CP_CMD_POOL_SIZE=${CONFIG_OSDP_CP_CMD_POOL_SIZE})
	zephyr_library_compile_definitions(OSDP_PD_SC_RETRY_MS=${CONFIG_OSDP_PD_SC_RETRY_MS})
	zephyr_library_compile_definitions(OSDP_PD_POLL_TIMEOUT_MS=${CONFIG_OSDP_PD_POLL_TIMEOUT_MS})
	zephyr_library_compile_definitions(OSDP_PD_SC_TIMEOUT_MS=${CONFIG_OSDP_PD_SC_TIMEOUT_MS})
	zephyr_library_compile_definitions(OSDP_PD_ONLINE_TOUT_MS=${CONFIG_OSDP_PD_ONLINE_TOUT_MS})
	zephyr_library_compile_definitions(OSDP_RESP_TOUT_MS=${CONFIG_OSDP_RESP_TOUT_MS})
	zephyr_library_compile_definitions(OSDP_CMD_MAX_RETRIES=${CONFIG_OSDP_CMD_MAX_RETRIES})
	zephyr_library_compile_definitions(OSDP_ONLINE_RETRY_WAIT_MAX_MS=${CONFIG_OSDP_ONLINE_RETRY_WAIT_MAX_MS})
	zephyr_library_compile_definitions(OSDP_CMD_RETRY_WAIT_MS=${CONFIG_OSDP_CMD_RETRY_WAIT_MS})
	zephyr_library_compile_definitions(OSDP_FILE_ERROR_RETRY_MAX=${CONFIG_OSDP_FILE_ERROR_RETRY_MAX})
	zephyr_library_compile_definitions(OSDP_PD_MAX=${CONFIG_OSDP_PD_MAX})
	zephyr_library_compile_definitions(OSDP_CMD_ID_OFFSET=${CONFIG_OSDP_CMD_ID_OFFSET})
	zephyr_library_compile_definitions(OSDP_PCAP_LINK_TYPE=${CONFIG_OSDP_PCAP_LINK_TYPE})
	zephyr_library_compile_definitions(OSDP_PD_NAME_MAXLEN=${CONFIG_OSDP_PD_NAME_MAXLEN})
	zephyr_library_compile_definitions(OSDP_CP_MAX_PDS=${CONFIG_OSDP_PD_MAX})

	zephyr_library_link_libraries(osdp)
	target_include_directories(modules_osdp PRIVATE ${OSDP_ROOT}/utils/include)

	# Feature toggles
	if (CONFIG_LIBOSDP_SKIP_MARK_BYTE)
		zephyr_library_compile_definitions(OPT_OSDP_SKIP_MARK_BYTE=1)
	endif()

	if (CONFIG_LIBOSDP_RX_ZERO_COPY)
		zephyr_library_compile_definitions(OPT_OSDP_RX_ZERO_COPY=1)
	endif()

	if (CONFIG_LIBOSDP_DISABLE_PRETTY_LOGGING)
		zephyr_library_compile_definitions(OPT_DISABLE_PRETTY_LOGGING=1)
	endif()

	if (CONFIG_LIBOSDP_STATIC)
		zephyr_library_compile_definitions(OPT_OSDP_STATIC=1)
	endif()

	if (CONFIG_LIBOSDP_LOG_MINIMAL)
		zephyr_library_compile_definitions(OPT_OSDP_LOG_MINIMAL=1)
	endif()

	if (CONFIG_LIBOSDP_PACKET_TRACE)
		zephyr_library_compile_definitions(OPT_OSDP_PACKET_TRACE=1)
	endif()

	if (CONFIG_LIBOSDP_DATA_TRACE)
		zephyr_library_compile_definitions(OPT_OSDP_DATA_TRACE=1)
	endif()

	# Core sources
	zephyr_library_sources(
		## LibOSDP
		${OSDP_ROOT}/src/osdp_cp.c
		${OSDP_ROOT}/src/osdp_pd.c
		${OSDP_ROOT}/src/osdp_common.c
		${OSDP_ROOT}/src/osdp_phy.c
		${OSDP_ROOT}/src/osdp_sc.c
		${OSDP_ROOT}/src/osdp_file.c
		${OSDP_ROOT}/src/osdp_metrics.c

		## Utils
		${OSDP_ROOT}/utils/src/list.c
		${OSDP_ROOT}/utils/src/queue.c
		${OSDP_ROOT}/utils/src/utils.c
		${OSDP_ROOT}/utils/src/disjoint_set.c

		## Zephyr glue code
		src/osdp.c
	)

	# Logger: full or minimal
	if (NOT CONFIG_LIBOSDP_LOG_MINIMAL)
		zephyr_library_sources(${OSDP_ROOT}/utils/src/logger.c)
	endif()

	# Diagnostics (packet/data trace → PCAP capture)
	if (CONFIG_LIBOSDP_PACKET_TRACE OR CONFIG_LIBOSDP_DATA_TRACE)
		zephyr_library_sources(
			${OSDP_ROOT}/src/osdp_diag.c
			${OSDP_ROOT}/utils/src/pcap_gen.c
		)
	endif()

	# Crypto backend (driven by the LIBOSDP_CRYPTO_BACKEND Kconfig choice)
	if (CONFIG_LIBOSDP_CRYPTO_MBEDTLS)
		message(STATUS "libosdp crypto backend: MbedTLS")
		zephyr_library_compile_definitions(OPT_OSDP_USE_MBEDTLS)
		zephyr_library_sources(${OSDP_ROOT}/src/crypto/mbedtls.c)
		target_include_directories(modules_osdp PRIVATE ${ZEPHYR_MBEDTLS_MODULE_DIR}/include)
		zephyr_link_libraries(mbedTLS)
	elseif(CONFIG_LIBOSDP_CRYPTO_TINYAES)
		message(STATUS "libosdp crypto backend: TinyAES (bundled)")
		zephyr_library_sources(
			${OSDP_ROOT}/src/crypto/tinyaes.c
			${OSDP_ROOT}/src/crypto/tinyaes_src.c
		)
	else()
		message(FATAL_ERROR
			"No LibOSDP crypto backend selected. "
			"Enable CONFIG_LIBOSDP_CRYPTO_MBEDTLS or CONFIG_LIBOSDP_CRYPTO_TINYAES.")
	endif()
endif()
