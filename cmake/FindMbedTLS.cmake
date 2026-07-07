find_path(MBEDTLS_INCLUDE_DIRS mbedtls/ssl.h)

find_library(MBEDTLS_LIBRARY mbedtls)
find_library(MBEDX509_LIBRARY mbedx509)
find_library(MBEDCRYPTO_LIBRARY mbedcrypto)

set(MBEDTLS_LIBRARIES "${MBEDTLS_LIBRARY}" "${MBEDX509_LIBRARY}" "${MBEDCRYPTO_LIBRARY}")

include(FindPackageHandleStandardArgs)

find_package_handle_standard_args(
	MbedTLS
	"Failed to find MbedTLS executable"
	MBEDTLS_INCLUDE_DIRS
	MBEDTLS_LIBRARY
	MBEDX509_LIBRARY
	MBEDCRYPTO_LIBRARY
)

if (MbedTLS_FOUND)
	if (NOT TARGET MbedTLS::mbedcrypto)
		add_library(MbedTLS::mbedcrypto UNKNOWN IMPORTED)
		set_target_properties(MbedTLS::mbedcrypto PROPERTIES
			IMPORTED_LOCATION "${MBEDCRYPTO_LIBRARY}"
			INTERFACE_INCLUDE_DIRECTORIES "${MBEDTLS_INCLUDE_DIRS}")
	endif()
	if (NOT TARGET MbedTLS::mbedx509)
		add_library(MbedTLS::mbedx509 UNKNOWN IMPORTED)
		set_target_properties(MbedTLS::mbedx509 PROPERTIES
			IMPORTED_LOCATION "${MBEDX509_LIBRARY}"
			INTERFACE_INCLUDE_DIRECTORIES "${MBEDTLS_INCLUDE_DIRS}"
			INTERFACE_LINK_LIBRARIES MbedTLS::mbedcrypto)
	endif()
	if (NOT TARGET MbedTLS::mbedtls)
		add_library(MbedTLS::mbedtls UNKNOWN IMPORTED)
		set_target_properties(MbedTLS::mbedtls PROPERTIES
			IMPORTED_LOCATION "${MBEDTLS_LIBRARY}"
			INTERFACE_INCLUDE_DIRECTORIES "${MBEDTLS_INCLUDE_DIRS}"
			INTERFACE_LINK_LIBRARIES "MbedTLS::mbedx509;MbedTLS::mbedcrypto")
	endif()
endif()

mark_as_advanced(
	MBEDTLS_INCLUDE_DIRS
	MBEDTLS_LIBRARY
	MBEDX509_LIBRARY
	MBEDCRYPTO_LIBRARY
)
