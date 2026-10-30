#!/usr/bin/env bash
#
# Copyright (c) 2026 Siddharth Chandrasekaran <sidcha@osdp.dev>
#
# SPDX-License-Identifier: Apache-2.0
#
# Builds the libosdp that language packages ship (NuGet today) for one .NET
# runtime identifier and lays it out as:
#
#   <outdir>/lib/      shared + static library
#   <outdir>/include/  public headers
#   <outdir>/abi.txt   what osdp_get_abi_info() reports for this build
#
# This is the only place that knows the shipped option set. CI runs it per RID.
#
# Env: BUILD_DIR, LIBOSDP_GIT_REV/LIBOSDP_GIT_TAG, ABI_PROBE_RUNNER,
#      SKIP_UNIT_TESTS=1, MAX_GLIBC=<x.y>

set -euo pipefail

if [ $# -ne 2 ]; then
	echo "usage: $0 <rid> <outdir>" >&2
	exit 2
fi

RID="$1"
OUT="$2"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${BUILD_DIR:-${ROOT}/build-native-${RID}}"
PREFIX="${BUILD}/install"

# The checkout may belong to another uid inside a container, where git refuses
# to describe it; resolve the revision here so the version string keeps it.
if [ -z "${LIBOSDP_GIT_REV:-}" ]; then
	LIBOSDP_GIT_REV="$(git -c safe.directory='*' -C "${ROOT}" \
		describe --tags --long --always --abbrev=7 2>/dev/null || true)"
	LIBOSDP_GIT_TAG="$(git -c safe.directory='*' -C "${ROOT}" \
		describe --exact-match --tags HEAD 2>/dev/null || true)"
fi

CMAKE_ARGS=(
	-DCMAKE_BUILD_TYPE=Release
	-DCMAKE_INSTALL_PREFIX="${PREFIX}"
	-DCMAKE_INSTALL_LIBDIR=lib
	-DCMAKE_POSITION_INDEPENDENT_CODE=ON
	-DOPT_BUILD_SHARED=ON
	-DOPT_BUILD_STATIC=ON
	-DOPT_OSDP_CRYPTO_BACKEND=tinyaes
	-DOPT_OSDP_RX_ZERO_COPY=OFF
	-DOPT_OSDP_LOG_MINIMAL=OFF
	-DOPT_OSDP_PACKET_TRACE=OFF
	-DOPT_OSDP_DATA_TRACE=OFF
	-DOPT_OSDP_STATIC=OFF
)
if [ -n "${LIBOSDP_GIT_REV}" ]; then
	CMAKE_ARGS+=(-DLIBOSDP_GIT_REV="${LIBOSDP_GIT_REV}"
		     -DLIBOSDP_GIT_TAG="${LIBOSDP_GIT_TAG:-}")
fi

RUN_TESTS=1
PROBE_ARGS=()
case "${RID}" in
win-x64 | win-arm64)
	# The Visual Studio generator defaults to the host architecture, which
	# would silently build the wrong DLL on a cross-arch runner.
	if [ "${RID}" = win-x64 ]; then VS_PLATFORM=x64; else VS_PLATFORM=ARM64; fi
	PROBE_ARGS+=(-A "${VS_PLATFORM}")
	# CMP0091 is OLD under cmake_minimum_required(3.14), which would make
	# CMake ignore CMAKE_MSVC_RUNTIME_LIBRARY and link the DLL CRT.
	CMAKE_ARGS+=(-A "${VS_PLATFORM}"
		     -DCMAKE_POLICY_DEFAULT_CMP0091=NEW
		     -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded
		     -DOPT_OSDP_LIB_ONLY=ON)
	RUN_TESTS=0 # `check` is gated off under MSVC
	;;
osx-arm64)
	CMAKE_ARGS+=(-DCMAKE_OSX_DEPLOYMENT_TARGET=12.0
		     -DCMAKE_OSX_ARCHITECTURES=arm64)
	PROBE_ARGS+=(-DCMAKE_OSX_ARCHITECTURES=arm64)
	;;
osx-x64)
	CMAKE_ARGS+=(-DCMAKE_OSX_DEPLOYMENT_TARGET=12.0
		     -DCMAKE_OSX_ARCHITECTURES=x86_64)
	PROBE_ARGS+=(-DCMAKE_OSX_ARCHITECTURES=x86_64)
	;;
linux-arm)
	# Cross-compiled; the toolchain comes from the environment (dockcross
	# exports CMAKE_TOOLCHAIN_FILE). Unit tests cannot run on the build host.
	# .NET's linux-arm is armhf; a soft-float build fails to load there.
	if ! echo | "${CC:-cc}" -dM -E - | grep __ARM_PCS_VFP >/dev/null; then
		echo "error: linux-arm needs a hard-float (armhf) toolchain" >&2
		exit 1
	fi
	CMAKE_ARGS+=(-DOPT_OSDP_LIB_ONLY=ON)
	RUN_TESTS=0
	;;
linux-x64 | linux-arm64 | linux-musl-x64 | linux-musl-arm64) ;;
*)
	echo "unknown rid: ${RID}" >&2
	exit 2
	;;
esac
if [ "${SKIP_UNIT_TESTS:-0}" = 1 ]; then
	RUN_TESTS=0
fi

cmake -S "${ROOT}" -B "${BUILD}" "${CMAKE_ARGS[@]}"
cmake --build "${BUILD}" --config Release --parallel
if [ "${RUN_TESTS}" = 1 ]; then
	cmake --build "${BUILD}" --config Release --target check
fi
cmake --install "${BUILD}" --config Release

PROBE_BUILD="${BUILD}/abi-probe"
cmake -S "${ROOT}/scripts/abi-probe" -B "${PROBE_BUILD}" \
	${PROBE_ARGS[@]+"${PROBE_ARGS[@]}"} \
	-DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="${PREFIX}"
cmake --build "${PROBE_BUILD}" --config Release

rm -rf "${OUT}"
mkdir -p "${OUT}/lib" "${OUT}/include"
cp -R "${PREFIX}/include/libosdp/." "${OUT}/include/"

# NuGet zips cannot carry symlinks: stage each library as one regular file
# under the name the .NET loader probes for.
case "${RID}" in
win-*)
	cp "${PREFIX}/bin/osdp.dll" "${OUT}/lib/osdp.dll"
	cp "${PREFIX}/lib/osdp.lib" "${OUT}/lib/osdp.lib"
	cp "${PREFIX}/lib/osdpstatic.lib" "${OUT}/lib/osdpstatic.lib"
	PROBE="${PROBE_BUILD}/Release/abi-probe.exe"
	cp "${OUT}/lib/osdp.dll" "$(dirname "${PROBE}")/"
	;;
osx-*)
	cp -L "${PREFIX}/lib/libosdp.dylib" "${OUT}/lib/libosdp.dylib"
	cp "${PREFIX}/lib/libosdpstatic.a" "${OUT}/lib/libosdpstatic.a"
	PROBE="${PROBE_BUILD}/abi-probe"
	minos="$(otool -l "${OUT}/lib/libosdp.dylib" | awk '/minos/ {print $2; exit}')"
	if [ "${minos}" != 12.0 ]; then
		echo "error: libosdp.dylib minos is '${minos}', expected 12.0" >&2
		exit 1
	fi
	;;
*)
	cp -L "${PREFIX}/lib/libosdp.so" "${OUT}/lib/libosdp.so"
	cp "${PREFIX}/lib/libosdpstatic.a" "${OUT}/lib/libosdpstatic.a"
	PROBE="${PROBE_BUILD}/abi-probe"
	;;
esac

for f in "${OUT}"/lib/*; do
	if [ -L "${f}" ] || [ ! -s "${f}" ]; then
		echo "error: ${f} is a symlink or empty" >&2
		exit 1
	fi
done

if [ -n "${MAX_GLIBC:-}" ]; then
	need="$(objdump -T "${OUT}/lib/libosdp.so" |
		grep -o 'GLIBC_[0-9.]*' | sed 's/GLIBC_//' | sort -V | tail -1)"
	if [ "$(printf '%s\n%s\n' "${need}" "${MAX_GLIBC}" | sort -V | tail -1)" != "${MAX_GLIBC}" ]; then
		echo "error: libosdp.so needs glibc ${need} > ${MAX_GLIBC}" >&2
		exit 1
	fi
fi

case "${RID}" in
linux-*) DEPS="$(objdump -p "${OUT}/lib/libosdp.so" | grep NEEDED || true)" ;;
osx-*) DEPS="$(otool -L "${OUT}/lib/libosdp.dylib")" ;;
*) DEPS="" ;;
esac
if printf '%s' "${DEPS}" | grep -q libcrypto; then
	echo "error: libosdp links libcrypto; tinyaes was not used" >&2
	exit 1
fi

{
	echo "rid=${RID}"
	LD_LIBRARY_PATH="${PREFIX}/lib" DYLD_LIBRARY_PATH="${PREFIX}/lib" \
		${ABI_PROBE_RUNNER:-} "${PROBE}"
} | tr -d '\r' >"${OUT}/abi.txt"

if ! grep -qx 'options=0x00000000' "${OUT}/abi.txt"; then
	echo "error: unexpected ABI options in ${OUT}/abi.txt" >&2
	cat "${OUT}/abi.txt" >&2
	exit 1
fi
cat "${OUT}/abi.txt"
