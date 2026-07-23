#!/usr/bin/env bash
#
#  Copyright (c) 2021-2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
#
#  SPDX-License-Identifier: Apache-2.0
#

usage() {
	cat >&2<<----
	LibOSDP build configure script

	Configures a bare minimum build environment for LibOSDP. This is not a
	replacement for cmake and intended only for those users who don't care
	about all the bells and whistles and need only the library.

	OPTIONS:
	  --packet-trace               Enable raw packet trace for diagnostics
	  --data-trace                 Enable command/reply data buffer tracing
	  --skip-mark                  Don't send the leading mark byte (0xFF)
	  --zero-copy                  Enable zero-copy RX buffers (requires recv_pkt/release_pkt)
	  --log-minimal                Minimize logger RAM/stack usage
	  --crypto LIB                 Crypto backend: auto|openssl|mbedtls|tinyaes (default: auto)
	  --crypto-include-dir DIR     Include directory for crypto LIB if not in system path
	  --crypto-ld-flags            Args to pass to linker for the crypto LIB
	  --no-colours                 Don't colourize log ouputs
	  --static                     Build without dynamic memory allocation
	  --static-pd                  Deprecated alias for --static
	  --lib-only                   Only build the library
	  --bare-metal                 Enable bare-metal build paths
	  --use-32bit-tick-t           Use uint32_t tick_t (requires --bare-metal)
	  --enable-trs                 Enable Transparent Reader Support (TRS)
	  --cross-compile PREFIX       Use to pass a compiler prefix
	  --prefix PATH                Install path prefix (default: /usr)
	  --build-dir                  Build output directory (default: ./build)
	  -d, --debug                  Enable debug builds
	  -f, --force                  Use this flags to override some checks
	  -h, --help                   Print this help
	---
}

BUILD_DIR="build"
SCRIPT_DIR="$(dirname $(readlink -f "$0"))"

while [ $# -gt 0 ]; do
	case $1 in
	--packet-trace)        PACKET_TRACE=1;;
	--data-trace)          DATA_TRACE=1;;
	--skip-mark)           SKIP_MARK_BYTE=1;;
	--zero-copy)           ZERO_COPY=1;;
	--log-minimal)         LOG_MINIMAL=1;;
	--cross-compile)       CROSS_COMPILE=$2; shift;;
	--prefix)              PREFIX=$2; shift;;
	--crypto)              CRYPTO=$2; shift;;
	--crypto-include-dir)  CRYPTO_INCLUDE_DIR=$2; shift;;
	--crypto-ld-flags)     CRYPTO_LD_FLAGS=$2; shift;;
	--no-colours)          NO_COLOURS=1;;
	--static|--static-pd)  STATIC=1;;
	--lib-only)            LIB_ONLY=1;;
	--bare-metal)          BARE_METAL=1;;
	--use-32bit-tick-t)    USE_32BIT_TICK_T=1;;
	--enable-trs)          ENABLE_TRS=1;;
	--build-dir)           BUILD_DIR=$2; shift;;
	-d|--debug)            DEBUG=1;;
	-f|--force)            FORCE=1;;
	-h|--help)             usage; exit 0;;
	*) echo -e "Unknown option $1\n"; usage; exit 1;;
	esac
	shift
done

mkdir -p ${BUILD_DIR}/include

if [ -f config.make ] && [ -z "$FORCE" ]; then
	echo "LibOSDP already configured! Use --force to re-configure"
	exit 1
fi

if [[ ("$OSTYPE" == "cygwin") || ("$OSTYPE" == "win32") ]]; then
	echo "Warning: unsupported platform. Expect issues!"
fi

## Toolchains
CC=${CC:-${CROSS_COMPILE}gcc}
CXX=${CXX:-${CROSS_COMPILE}g++}
AR=${AR:-${CROSS_COMPILE}ar}

## check if submodules are initialized
if [[ -z "$(ls -A utils)" ]]; then
	"Initializing git-submodule utils"
	git submodule update --init
fi

## Build options
if [[ ! -z "${NO_COLOURS}" ]]; then
	CCFLAGS+=" -DOPT_DISABLE_PRETTY_LOGGING"
fi

if [[ ! -z "${PACKET_TRACE}" ]]; then
	CCFLAGS+=" -DOPT_OSDP_PACKET_TRACE"
fi

if [[ ! -z "${DATA_TRACE}" ]]; then
	CCFLAGS+=" -DOPT_OSDP_DATA_TRACE"
fi

if [[ ! -z "${SKIP_MARK_BYTE}" ]]; then
	CCFLAGS+=" -DOPT_OSDP_SKIP_MARK_BYTE"
fi

if [[ ! -z "${ZERO_COPY}" ]]; then
	CCFLAGS+=" -DOPT_OSDP_RX_ZERO_COPY"
fi

if [[ ! -z "${LOG_MINIMAL}" ]]; then
	CCFLAGS+=" -DOPT_OSDP_LOG_MINIMAL"
fi

if [[ ! -z "${STATIC}" ]]; then
	CCFLAGS+=" -DOPT_OSDP_STATIC"
fi

if [[ ! -z "${DEBUG}" ]]; then
	CCFLAGS+=" -g"
fi

if [[ ! -z "${BARE_METAL}" ]]; then
	CCFLAGS+=" -D__BARE_METAL__"
fi

if [[ ! -z "${USE_32BIT_TICK_T}" ]]; then
	if [[ -z "${BARE_METAL}" ]]; then
		echo "--use-32bit-tick-t requires --bare-metal"
		exit 1
	fi
	CCFLAGS+=" -DUSE_32BIT_TICK_T"
fi

if [[ ! -z "${ENABLE_TRS}" ]]; then
	CCFLAGS+=" -DOPT_BUILD_OSDP_TRS"
fi

## Repo meta data
echo "Extracting source code meta information"
PROJECT_VERSION=$(perl -ne 'print if s/^project\(libosdp VERSION ([0-9.]+)\)$/\1/' CMakeLists.txt)
if [[ -z "${PROJECT_VERSION}" ]]; then
	echo "Failed to parse PROJECT_VERSION!"
	exit -1
fi

PROJECT_NAME=libosdp
GIT_DESC=""
if [[ -d .git ]]; then
	GIT_BRANCH=$(git rev-parse --abbrev-ref HEAD)
	GIT_REV=$(git log --pretty=format:'%h' -n 1)
	GIT_TAG=$(git describe --exact-match --tags 2>/dev/null)
	GIT_DIFF=$(git diff --quiet --exit-code || echo +)
	GIT_DESC=$(git describe --tags --long --always --abbrev=7 2>/dev/null)
fi

# Compose the human-visible version string; mirrors cmake/GitInfo.cmake so the
# lean and CMake builds report the same osdp_get_version() string.
LIBOSDP_PRERELEASE=$(perl -ne 'print $1 if /^set\(LIBOSDP_PRERELEASE "([^"]*)"\)/' CMakeLists.txt)
if [[ -z "${LIBOSDP_PRERELEASE}" ]]; then
	LIBOSDP_VERSION_STR="${PROJECT_VERSION}"
else
	LIBOSDP_VERSION_STR="${PROJECT_VERSION}-${LIBOSDP_PRERELEASE}"
	if [[ "${GIT_DESC}" =~ -([0-9]+)-g([0-9a-f]+)$ ]]; then
		LIBOSDP_VERSION_STR+=".${BASH_REMATCH[1]}+g${BASH_REMATCH[2]}"
	elif [[ -n "${GIT_DESC}" ]]; then
		LIBOSDP_VERSION_STR+="+g${GIT_DESC}"
	fi
fi

## Crypto backend selection: auto probes openssl → mbedtls → tinyaes by
## looking for the backend's public header through the compiler's default
## include path. Explicit names skip the probe and are used as-is.
probe_header() {
	echo "#include <$1>" | ${CC} -xc -E - > /dev/null 2>&1
}

CRYPTO=${CRYPTO:-auto}
if [[ "${CRYPTO}" == "auto" ]]; then
	if probe_header openssl/evp.h; then
		CRYPTO=openssl
	elif probe_header mbedtls/aes.h; then
		CRYPTO=mbedtls
	else
		CRYPTO=tinyaes
	fi
fi

case "${CRYPTO}" in
openssl)
	echo "Crypto backend: OpenSSL"
	LIBOSDP_SOURCES+=" src/crypto/openssl.c"
	LDFLAGS+=" -lcrypto"
	;;
mbedtls)
	echo "Crypto backend: MbedTLS"
	LIBOSDP_SOURCES+=" src/crypto/mbedtls.c"
	LDFLAGS+=" -lmbedcrypto -lmbedtls"
	;;
tinyaes)
	echo "Crypto backend: TinyAES (bundled)"
	LIBOSDP_SOURCES+=" src/crypto/tinyaes_src.c src/crypto/tinyaes.c"
	;;
*)
	echo "--crypto must be one of: auto, openssl, mbedtls, tinyaes (got '${CRYPTO}')"
	exit 1
	;;
esac

if [[ ! -z "${CRYPTO_INCLUDE_DIR}" ]]; then
	CCFLAGS+=" -I${CRYPTO_INCLUDE_DIR}"
	CXXFLAGS+=" -I${CRYPTO_INCLUDE_DIR}"
fi

if [[ ! -z "${CRYPTO_LD_FLAGS}" ]]; then
	LDFLAGS+=" -I${CRYPTO_LD_FLAGS}"
fi

## Declare sources
LIBOSDP_SOURCES+=" src/osdp_common.c src/osdp_phy.c src/osdp_sc.c src/osdp_file.c src/osdp_pd.c"
LIBOSDP_SOURCES+=" src/osdp_cp.c src/osdp_metrics.c src/osdp_multipart.c"
LIBOSDP_SOURCES+=" src/osdp_piv.c src/osdp_bio.c"
LIBOSDP_SOURCES+=" utils/src/list.c utils/src/queue.c utils/src/utils.c"
LIBOSDP_SOURCES+=" utils/src/disjoint_set.c utils/src/crc16.c"
if [[ -z "${LOG_MINIMAL}" ]]; then
	LIBOSDP_SOURCES+=" utils/src/logger.c"
fi

UTILS_SOURCES+=" utils/src/workqueue.c utils/src/circbuf.c utils/src/event.c utils/src/fdutils.c"

if [[ ! -z "${PACKET_TRACE}" ]] || [[ ! -z "${DATA_TRACE}" ]]; then
	LIBOSDP_SOURCES+=" src/osdp_diag.c utils/src/pcap_gen.c"
fi

TARGETS="cp_app pd_app"

if [[ ! -z "${ENABLE_TRS}" ]]; then
	LIBOSDP_SOURCES+=" src/osdp_trs.c"
fi

# Suite source files come from the shared manifest so the lean and CMake
# builds never drift; see tests/unit-tests/test-sources.list.
TEST_SOURCES=""
for f in $(grep '\.c$' tests/unit-tests/test-sources.list); do
	TEST_SOURCES+=" tests/unit-tests/$f"
done
TEST_SOURCES+=" ${LIBOSDP_SOURCES} ${UTILS_SOURCES}"

if [[ ! -z "${LIB_ONLY}" ]]; then
	TARGETS=""
fi

if [[ -z "${PREFIX}" ]]; then
	PREFIX="/usr"
fi

## Generate libosdp.pc
echo "Generating libosdp.pc"
sed -e "s|@CMAKE_INSTALL_PREFIX@|${PREFIX}|" \
    -e "s|@PROJECT_NAME@|${PROJECT_NAME}|" \
    -e 's|@PROJECT_DESCRIPTION@|Open Supervised Device Protocol (OSDP) Library|' \
    -e "s|@PROJECT_URL@|https://github.com/osdp-dev/libosdp|" \
    -e "s|@PROJECT_VERSION@|${PROJECT_VERSION}|" \
    -e "s|@LIB_TARGET@|osdp|" \
    -e "s|@LIBOSDP_PC_LIBDIR_REL@|lib|" \
    -e "s|@LIBOSDP_PC_REQUIRES_PRIVATE@||" \
	misc/libosdp.pc.in > ${BUILD_DIR}/libosdp.pc

## Generate osdp_config.h
echo "Generating osdp_config.h"
sed -e "s|@PROJECT_VERSION@|${PROJECT_VERSION}|" \
    -e "s|@LIBOSDP_VERSION_STR@|${LIBOSDP_VERSION_STR}|" \
    -e "s|@PROJECT_NAME@|${PROJECT_NAME}|" \
    -e "s|@GIT_BRANCH@|${GIT_BRANCH}|" \
    -e "s|@GIT_REV@|${GIT_REV}|" \
    -e "s|@GIT_TAG@|${GIT_TAG}|" \
    -e "s|@GIT_DIFF@|${GIT_DIFF}|" \
    -e "s|@REPO_ROOT@|${SCRIPT_DIR}|" \
	src/osdp_config.h.in > ${BUILD_DIR}/include/osdp_config.h

CCFLAGS+=" -I${BUILD_DIR}/include"

## Generate Makefile
echo "Generating config.make"
cat > config.make << ---
CC=${CC}
CXX=${CXX}
AR=${AR}
SYSROOT=${SYSROOT}
CCFLAGS=${CCFLAGS}
CXXFLAGS=${CXXFLAGS}
LDFLAGS=${LDFLAGS}
SRC_LIBOSDP=${LIBOSDP_SOURCES}
SRC_TEST=${TEST_SOURCES}
TARGETS=${TARGETS}
BUILD_DIR=$(realpath ${BUILD_DIR})
PREFIX=${PREFIX}
---
echo
echo "LibOSDP lean build system configured!"
echo "Invoke 'make' to build."
