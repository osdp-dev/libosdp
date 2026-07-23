#
#  Copyright (c) 2020-2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
#
#  SPDX-License-Identifier: Apache-2.0
#

if (NOT OPT_BUILD_STATIC AND NOT OPT_BUILD_SHARED)
	message(FATAL_ERROR "Both static and shared builds must not be disabled")
endif()

if (OPT_OSDP_PACKET_TRACE)
	list(APPEND LIB_OSDP_DEFINITIONS "-DOPT_OSDP_PACKET_TRACE=1")
endif()

if (OPT_OSDP_DATA_TRACE)
	list(APPEND LIB_OSDP_DEFINITIONS "-DOPT_OSDP_DATA_TRACE=1")
endif()

if (OPT_OSDP_SKIP_MARK_BYTE)
	list(APPEND LIB_OSDP_DEFINITIONS "-DOPT_OSDP_SKIP_MARK_BYTE=1")
endif()

if (OPT_OSDP_RX_ZERO_COPY)
	list(APPEND LIB_OSDP_DEFINITIONS "-DOPT_OSDP_RX_ZERO_COPY=1")
endif()

if (OPT_DISABLE_PRETTY_LOGGING)
	list(APPEND LIB_OSDP_DEFINITIONS "-DOPT_DISABLE_PRETTY_LOGGING=1")
endif()

if (OPT_OSDP_LOG_MINIMAL)
	list(APPEND LIB_OSDP_DEFINITIONS "-DOPT_OSDP_LOG_MINIMAL=1")
endif()

if (OPT_OSDP_STATIC)
	list(APPEND LIB_OSDP_DEFINITIONS "-DOPT_OSDP_STATIC=1")
endif()

if (OPT_BUILD_OSDP_TRS)
	list(APPEND LIB_OSDP_DEFINITIONS "-DOPT_BUILD_OSDP_TRS")
endif()

# Crypto backend selection driven by OPT_OSDP_CRYPTO_BACKEND:
#   auto    - probe openssl, then mbedtls, else fall back to bundled tinyaes
#   openssl - require OpenSSL (hard-fail if missing)
#   mbedtls - require MbedTLS (hard-fail if missing)
#   tinyaes - force bundled implementation; skip find_package entirely
set(OpenSSL_FOUND FALSE)
set(MbedTLS_FOUND FALSE)

if (OPT_OSDP_CRYPTO_BACKEND STREQUAL "auto")
	find_package(OpenSSL)
	if (NOT OpenSSL_FOUND)
		find_package(MbedTLS)
	endif()
elseif (OPT_OSDP_CRYPTO_BACKEND STREQUAL "openssl")
	find_package(OpenSSL REQUIRED)
elseif (OPT_OSDP_CRYPTO_BACKEND STREQUAL "mbedtls")
	find_package(MbedTLS REQUIRED)
elseif (NOT OPT_OSDP_CRYPTO_BACKEND STREQUAL "tinyaes")
	message(FATAL_ERROR
		"OPT_OSDP_CRYPTO_BACKEND must be one of: auto, openssl, mbedtls, tinyaes "
		"(got '${OPT_OSDP_CRYPTO_BACKEND}')")
endif()

if (OpenSSL_FOUND)
	message(STATUS "libosdp crypto backend: OpenSSL")
elseif (MbedTLS_FOUND)
	message(STATUS "libosdp crypto backend: MbedTLS")
elseif (OPT_OSDP_CRYPTO_BACKEND STREQUAL "auto")
	message(WARNING
		"libosdp crypto backend: TinyAES (bundled). No system crypto "
		"library was found in 'auto' mode; install OpenSSL or MbedTLS, "
		"or set OPT_OSDP_CRYPTO_BACKEND=tinyaes explicitly to silence "
		"this warning.")
else()
	message(STATUS "libosdp crypto backend: TinyAES (bundled)")
endif()

# Generate osdp_config.h in build dir.
set(REPO_ROOT ${CMAKE_SOURCE_DIR})
configure_file(osdp_config.h.in ${PROJECT_BINARY_DIR}/include/osdp_config.h @ONLY)

list(APPEND LIB_OSDP_SOURCES
	${CMAKE_CURRENT_SOURCE_DIR}/osdp_common.c
	${CMAKE_CURRENT_SOURCE_DIR}/osdp_phy.c
	${CMAKE_CURRENT_SOURCE_DIR}/osdp_pd.c
	${CMAKE_CURRENT_SOURCE_DIR}/osdp_cp.c
	${CMAKE_CURRENT_SOURCE_DIR}/osdp_sc.c
	${CMAKE_CURRENT_SOURCE_DIR}/osdp_file.c
	${CMAKE_CURRENT_SOURCE_DIR}/osdp_multipart.c
	${CMAKE_CURRENT_SOURCE_DIR}/osdp_piv.c
	${CMAKE_CURRENT_SOURCE_DIR}/osdp_bio.c
	${CMAKE_CURRENT_SOURCE_DIR}/osdp_metrics.c
)

if (OPT_OSDP_PACKET_TRACE OR OPT_OSDP_DATA_TRACE)
	list(APPEND LIB_OSDP_SOURCES
		${CMAKE_CURRENT_SOURCE_DIR}/osdp_diag.c
	)
endif()

if (OPT_BUILD_OSDP_TRS)
	list(APPEND LIB_OSDP_SOURCES
		${CMAKE_CURRENT_SOURCE_DIR}/osdp_trs.c
	)
endif()

list(APPEND LIB_OSDP_INCLUDE_DIRS
	${PROJECT_BINARY_DIR}/include
)

list(APPEND LIB_OSDP_PRIVATE_INCLUDE_DIRS
	${CMAKE_CURRENT_SOURCE_DIR}
	${CMAKE_CURRENT_BINARY_DIR}
)

if (OpenSSL_FOUND)
	list(APPEND LIB_OSDP_LIBRARIES ${OPENSSL_CRYPTO_LIBRARY}) # Needed for python library building
	list(APPEND LIB_OSDP_INCLUDE_DIRS ${OPENSSL_INCLUDE_DIR})
	list(APPEND LIB_OSDP_DEFINITIONS "-DOPT_OSDP_USE_OPENSSL")
	list(APPEND LIB_OSDP_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/crypto/openssl.c)
elseif (MbedTLS_FOUND)
	list(APPEND LIB_OSDP_DEFINITIONS "-DOPT_OSDP_USE_MBEDTLS")
	list(APPEND LIB_OSDP_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/crypto/mbedtls.c)
else()
	list(APPEND LIB_OSDP_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/crypto/tinyaes.c)
	list(APPEND LIB_OSDP_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/crypto/tinyaes_src.c)
endif()

# For shared library (gcc/linux), utils must be recompiled with -fPIC. Right
# now cmake doesn't support `--whole-archvive ... --no-whole-archive` directive
# to linker (see https://gitlab.kitware.com/cmake/cmake/-/issues/20078).
#
# Note: Mac seems to be impervious to this issue, I suppose it treats all
# objects relocatable by default (?).
#
list(APPEND LIB_OSDP_UTILS_SRC
	${PROJECT_SOURCE_DIR}/utils/src/utils.c
	${PROJECT_SOURCE_DIR}/utils/src/list.c
	${PROJECT_SOURCE_DIR}/utils/src/queue.c
	${PROJECT_SOURCE_DIR}/utils/src/disjoint_set.c
	${PROJECT_SOURCE_DIR}/utils/src/crc16.c
)
if (NOT OPT_OSDP_LOG_MINIMAL)
	list(APPEND LIB_OSDP_UTILS_SRC
		${PROJECT_SOURCE_DIR}/utils/src/logger.c
	)
endif()

list(APPEND LIB_OSDP_HEADERS
	${PROJECT_SOURCE_DIR}/include/osdp.h
	${PROJECT_SOURCE_DIR}/include/osdp.hpp
	${PROJECT_SOURCE_DIR}/include/osdp_export.h
)

# These variables are used in other parts of this projects for rebuilding
# LibOSDP as they see fit. For instance pyosdp is built by setuptools.
set(LIB_OSDP_SOURCES ${LIB_OSDP_SOURCES} PARENT_SCOPE)
set(LIB_OSDP_HEADERS ${LIB_OSDP_HEADERS} PARENT_SCOPE)
set(LIB_OSDP_LIBRARIES ${LIB_OSDP_LIBRARIES} PARENT_SCOPE)
set(LIB_OSDP_INCLUDE_DIRS ${LIB_OSDP_INCLUDE_DIRS} PARENT_SCOPE)
set(LIB_OSDP_PRIVATE_INCLUDE_DIRS ${LIB_OSDP_PRIVATE_INCLUDE_DIRS} PARENT_SCOPE)
set(LIB_OSDP_DEFINITIONS ${LIB_OSDP_DEFINITIONS} PARENT_SCOPE)

add_definitions(${LIB_OSDP_DEFINITIONS})

################################################
##
## Build Static Library (OPT_BUILD_SHARED=on)
##
################################################
if (OPT_BUILD_STATIC)

set(LIB_OSDP_STATIC osdpstatic)
set(LIB_TARGET ${LIB_OSDP_STATIC}) ## to be used in libosdp.pc.in
add_library(${LIB_OSDP_STATIC} STATIC ${LIB_OSDP_SOURCES} ${LIB_OSDP_UTILS_SRC})
add_library(libosdp::osdpstatic ALIAS ${LIB_OSDP_STATIC})
if (OpenSSL_FOUND)
	target_link_libraries(${LIB_OSDP_STATIC} PUBLIC OpenSSL::Crypto)
elseif (MbedTLS_FOUND)
	target_link_libraries(${LIB_OSDP_STATIC} PUBLIC MbedTLS::mbedcrypto)
endif()

set_target_properties(${LIB_OSDP_STATIC} PROPERTIES
	VERSION ${PROJECT_VERSION}
	COMPILE_FLAGS -DOSDP_STATIC_DEFINE
	PUBLIC_HEADER "${LIB_OSDP_HEADERS}"
	C_VISIBILITY_PRESET hidden
	CXX_VISIBILITY_PRESET hidden
	VISIBILITY_INLINES_HIDDEN YES
)

target_include_directories(${LIB_OSDP_STATIC}
	PUBLIC
		$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
		$<BUILD_INTERFACE:${PROJECT_BINARY_DIR}/include>
		$<INSTALL_INTERFACE:include/libosdp>
	PRIVATE
		${LIB_OSDP_INCLUDE_DIRS}
		${LIB_OSDP_PRIVATE_INCLUDE_DIRS}
		${PROJECT_SOURCE_DIR}/utils/include
)

if (OPT_BUILD_SANITIZER)
	target_compile_options(${LIB_OSDP_STATIC} PRIVATE
		-fsanitize=address,undefined,leak
	)
	target_link_options(${LIB_OSDP_STATIC} PRIVATE
		-fsanitize=address,undefined,leak
	)
endif()

install(
	TARGETS ${LIB_OSDP_STATIC}
	COMPONENT distributables
	EXPORT LibOSDPTargets
	LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
	ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

endif() # OPT_BUILD_STATIC

################################################
##
## Build Shared Library (OPT_BUILD_SHARED=on)
##
################################################
if (OPT_BUILD_SHARED)

set(LIB_OSDP_SHARED osdp)
set(LIB_TARGET ${LIB_OSDP_SHARED}) ## to be used in libosdp.pc.in
# keep all symbols hidden by default (-fvisibility=hidden) for shared library
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_C_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN 1)

if (OPT_OSDP_PACKET_TRACE OR OPT_OSDP_DATA_TRACE)
	list(APPEND LIB_OSDP_UTILS_SRC
		${PROJECT_SOURCE_DIR}/utils/src/pcap_gen.c
	)
endif()

set(LIB_OSDP_UTILS_SRC ${LIB_OSDP_UTILS_SRC} PARENT_SCOPE)

add_library(${LIB_OSDP_SHARED} SHARED ${LIB_OSDP_SOURCES} ${LIB_OSDP_UTILS_SRC})
add_library(libosdp::osdp ALIAS ${LIB_OSDP_SHARED})
if (OpenSSL_FOUND)
	target_link_libraries(${LIB_OSDP_SHARED} PUBLIC OpenSSL::Crypto)
elseif (MbedTLS_FOUND)
	target_link_libraries(${LIB_OSDP_SHARED} PUBLIC MbedTLS::mbedcrypto)
endif()

set_target_properties(${LIB_OSDP_SHARED} PROPERTIES
	VERSION ${PROJECT_VERSION}
	SOVERSION ${PROJECT_VERSION_MAJOR}
	COMPILE_FLAGS -DBUILDING_API
	PUBLIC_HEADER "${LIB_OSDP_HEADERS}"
	C_VISIBILITY_PRESET hidden
	CXX_VISIBILITY_PRESET hidden
	VISIBILITY_INLINES_HIDDEN YES
)

target_include_directories(${LIB_OSDP_SHARED}
	PUBLIC
		$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
		$<BUILD_INTERFACE:${PROJECT_BINARY_DIR}/include>
		$<INSTALL_INTERFACE:include/libosdp>
	PRIVATE
		${LIB_OSDP_PRIVATE_INCLUDE_DIRS}
		${PROJECT_SOURCE_DIR}/utils/include
)

if (OPT_BUILD_SANITIZER)
	target_compile_options(${LIB_OSDP_SHARED} PRIVATE
		-fsanitize=address,undefined,leak
	)
	target_link_options(${LIB_OSDP_SHARED} PRIVATE
		-fsanitize=address,undefined,leak
	)
endif()

install(
	TARGETS ${LIB_OSDP_SHARED}
	EXPORT LibOSDPTargets
	LIBRARY
		COMPONENT distributables
		DESTINATION ${CMAKE_INSTALL_LIBDIR}
	ARCHIVE
		COMPONENT distributables
		DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

endif() # OPT_BUILD_SHARED

# Headers are installed once, regardless of how many lib variants were built.
# Avoids duplicate manifest entries that broke the uninstall target.
install(FILES ${LIB_OSDP_HEADERS}
	DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/libosdp
	COMPONENT headers
)

# Default alias: pick shared if available, else static. Lets consumers
# (both find_package and add_subdirectory) write
# `target_link_libraries(... libosdp::libosdp)` without caring which
# variant was built.
if (OPT_BUILD_SHARED)
	add_library(libosdp::libosdp ALIAS ${LIB_OSDP_SHARED})
elseif (OPT_BUILD_STATIC)
	add_library(libosdp::libosdp ALIAS ${LIB_OSDP_STATIC})
endif()

if (NOT OPT_BUILD_BARE_METAL)

# pkg-config file. LIBOSDP_PC_LIBDIR_REL/REQUIRES_PRIVATE are substituted
# into misc/libosdp.pc.in. The lean Make build (configure.sh) substitutes
# the same tokens with simpler defaults.
set(LIBOSDP_PC_LIBDIR_REL "${CMAKE_INSTALL_LIBDIR}")
if (OpenSSL_FOUND)
	set(LIBOSDP_PC_REQUIRES_PRIVATE "openssl")
elseif (MbedTLS_FOUND)
	set(LIBOSDP_PC_REQUIRES_PRIVATE "mbedcrypto")
else()
	set(LIBOSDP_PC_REQUIRES_PRIVATE "")
endif()
configure_file(
	${PROJECT_SOURCE_DIR}/misc/libosdp.pc.in
	${CMAKE_BINARY_DIR}/libosdp.pc @ONLY
)

install(FILES ${CMAKE_BINARY_DIR}/libosdp.pc
	DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig
	COMPONENT config_files
)

## Package Configuration. Names follow `find_package(libosdp)` convention:
## libosdpConfig.cmake / libosdpConfigVersion.cmake / libosdpTargets.cmake.

include(CMakePackageConfigHelpers)
install(EXPORT LibOSDPTargets
	FILE libosdpTargets.cmake
	NAMESPACE libosdp::
	DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/libosdp
	COMPONENT config_files
)

# Compose find_dependency block for the consumer-side libosdpConfig.cmake.
# When libosdp is built against MbedTLS, also ship our FindMbedTLS.cmake
# alongside the package config so the consumer's find_dependency() resolves.
if (OpenSSL_FOUND)
	set(LIBOSDP_FIND_DEPENDENCY_BLOCK "find_dependency(OpenSSL)")
elseif (MbedTLS_FOUND)
	set(LIBOSDP_FIND_DEPENDENCY_BLOCK
		"list(APPEND CMAKE_MODULE_PATH \"\${CMAKE_CURRENT_LIST_DIR}\")\nfind_dependency(MbedTLS)")
	install(FILES "${PROJECT_SOURCE_DIR}/cmake/FindMbedTLS.cmake"
		DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/libosdp"
		COMPONENT config_files
	)
else()
	set(LIBOSDP_FIND_DEPENDENCY_BLOCK "")
endif()

configure_package_config_file(
	${PROJECT_SOURCE_DIR}/cmake/libosdpConfig.cmake.in
	${CMAKE_BINARY_DIR}/libosdpConfig.cmake
	INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/libosdp
	NO_CHECK_REQUIRED_COMPONENTS_MACRO
	INSTALL_PREFIX ${CMAKE_INSTALL_PREFIX}
)
write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/libosdpConfigVersion.cmake"
	VERSION ${PROJECT_VERSION}
	COMPATIBILITY SameMajorVersion
)
install(FILES
		"${CMAKE_BINARY_DIR}/libosdpConfig.cmake"
		"${CMAKE_CURRENT_BINARY_DIR}/libosdpConfigVersion.cmake"
	DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/libosdp
	COMPONENT config_files
)

endif() # NOT OPT_BUILD_BARE_METAL
