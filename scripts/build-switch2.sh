#!/usr/bin/env bash
# Builds Vita3K for the Nintendo Switch 2 (kaiHEN homebrew):
#   build-switch2/Vita3K-NX2.nro          the emulator core
#   switch-launcher/Vita3K-NX2.nro        the launcher, which bundles the core (copy it to sdmc:/switch/vita3k/)
#
#   bash scripts/build-switch2.sh [--core-only] [clean]
#
# Run it under devkitPro's MSYS2 with ouncebrew's ounce-devkit installed in $DEVKITPRO (libounce, libkaihen and the
# Switch 2 CMake toolchain and Makefile rules). The Switch build is unaffected: it keeps its own directories
# (build-switch-*, switch-launcher/build) and output names (Vita3K.nro).
#
# Environment:
#   SWITCH2_MESA_SDK_ZIP       mesa-26.2.7-switch2-horizon-sdk.zip (default: ../mesa-nx/dist/ next to this checkout)
#   VITA3K_SWITCH_OPENSSL_DIR  the OpenSSL 4.0.1 build from scripts/prepare-switch-openssl.sh (default
#                              build-switch-deps/openssl-4.0.1-armv8-crypto; plain ARMv8 code shared with the Switch build)
#   JOBS                       parallel jobs (default: all CPUs)
#
# There is no drako3d renderer in Vita3K: the core renders with NVK (Vulkan) or Mesa's GL.
set -euo pipefail

# MSYS2's cmake, ninja and make (devkitPro's CMake files refuse a CLANGARM64 shell's).
export PATH="/usr/bin:/bin:${PATH:-}"

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
BUILD_DIR="${ROOT}/build-switch2"
DEPS_DIR="${ROOT}/build-switch2-deps"
LAUNCHER_DIR="${ROOT}/switch-launcher"
CORE_ONLY=0
CLEAN=0

for argument in "$@"; do
	case "${argument}" in
		--core-only) CORE_ONLY=1 ;;
		clean) CLEAN=1 ;;
		-h|--help)
			sed -n '2,20p' "$0"
			exit 0
			;;
		*)
			echo "Usage: $0 [--core-only] [clean]" >&2
			exit 2
			;;
	esac
done

: "${DEVKITPRO:=/opt/devkitpro}"
export DEVKITPRO
export DEVKITA64="${DEVKITA64:-${DEVKITPRO}/devkitA64}"
# (portlibs/switch2/bin first: its aarch64-none-elf-pkg-config is the Switch 2 toolchain's)
export PATH="${DEVKITPRO}/portlibs/switch2/bin:${DEVKITA64}/bin:${DEVKITPRO}/tools/bin:${PATH}"

TOOLCHAIN="${DEVKITPRO}/libounce/cmake/Switch2.cmake"
[[ -f "${TOOLCHAIN}" ]] || { echo "No Switch 2 devkit in ${DEVKITPRO}: install https://github.com/ouncebrew/ounce-devkit" >&2; exit 1; }
[[ -f "${DEVKITPRO}/libounce/switch2_rules" ]] || { echo "libounce's switch2_rules is missing from ${DEVKITPRO}/libounce" >&2; exit 1; }
[[ -f "${DEVKITPRO}/portlibs/switch2/lib/libkaihen.a" ]] || { echo "libkaihen (kaiHEN's JIT provider) is not installed in portlibs/switch2" >&2; exit 1; }

JOBS="${JOBS:-$(nproc 2>/dev/null || echo 8)}"
case "${JOBS}" in
	''|*[!0-9]*|0) echo "JOBS must be a positive integer" >&2; exit 2 ;;
esac

if ((CLEAN)); then
	rm -rf -- "${BUILD_DIR}" "${LAUNCHER_DIR}/build-switch2" "${LAUNCHER_DIR}/dependencies/build-switch2"
	rm -f -- "${LAUNCHER_DIR}/Vita3K-NX2.nro" "${LAUNCHER_DIR}/Vita3K-NX2.elf" "${LAUNCHER_DIR}/Vita3K-NX2.nacp" \
		"${LAUNCHER_DIR}/Vita3K-NX2.lst" "${LAUNCHER_DIR}/Vita3K-NX2.map"
	echo "Removed the Switch 2 build directories"
	exit 0
fi

for tool in cmake ninja make sha256sum cygpath aarch64-none-elf-gcc; do
	command -v "${tool}" >/dev/null || { echo "Missing build tool: ${tool}" >&2; exit 1; }
done

mkdir -p "${BUILD_DIR}/tmp" "${DEPS_DIR}"
# devkitA64's gcc.exe (LTO in particular) needs a writable Windows temporary directory.
export TMPDIR="${BUILD_DIR}/tmp"
TEMP="$(cygpath -m "${TMPDIR}")"
export TEMP
export TMP="${TEMP}"

# The Switch 2 Mesa SDK (verified and extracted once).
MESA_SDK_ZIP="${SWITCH2_MESA_SDK_ZIP:-${ROOT}/../mesa-nx/dist/mesa-26.2.7-switch2-horizon-sdk.zip}"
MESA_SDK="$(bash "${ROOT}/scripts/prepare-switch2-mesa-sdk.sh" --build-root "${DEPS_DIR}" --archive "${MESA_SDK_ZIP}" | tail -n 1)"
[[ -d "${MESA_SDK}" ]] || { echo "Could not prepare the Switch 2 Mesa SDK" >&2; exit 1; }

OPENSSL_DIR="${VITA3K_SWITCH_OPENSSL_DIR:-${ROOT}/build-switch-deps/openssl-4.0.1-armv8-crypto}"
[[ -f "${OPENSSL_DIR}/libcrypto.a" ]] || {
	echo "No Switch OpenSSL build at ${OPENSSL_DIR}: run scripts/prepare-switch-openssl.sh or set VITA3K_SWITCH_OPENSSL_DIR" >&2
	exit 1
}

cmake -S "${ROOT}" -B "${BUILD_DIR}" -G Ninja \
	-DCMAKE_TOOLCHAIN_FILE="${TOOLCHAIN}" \
	-DCMAKE_BUILD_TYPE=Release \
	-DSWITCH2_MESA_SDK="${MESA_SDK}" \
	-DSWITCH_OPENSSL_DIR="${OPENSSL_DIR}"
cmake --build "${BUILD_DIR}" --target vita3k_nro --parallel "${JOBS}"

CORE_NRO="${BUILD_DIR}/Vita3K-NX2.nro"
[[ -f "${CORE_NRO}" ]] || { echo "The build finished without ${CORE_NRO}" >&2; exit 1; }
echo "Built the Switch 2 core: ${CORE_NRO}"
sha256sum "${CORE_NRO}"
((CORE_ONLY)) && exit 0

# The launcher (switch-launcher/Makefile with SWITCH2=1: libounce's switch2_rules, Vita3K-NX2.nro, its own build
# and RomFS directories), bundling the core.
make -C "${LAUNCHER_DIR}" -j"${JOBS}" \
	SWITCH2=1 \
	SWITCH2_MESA_SDK="${MESA_SDK}" \
	SWITCH2_EMU_NRO="${CORE_NRO}"

LAUNCHER_NRO="${LAUNCHER_DIR}/Vita3K-NX2.nro"
[[ -f "${LAUNCHER_NRO}" ]] || { echo "The launcher build finished without ${LAUNCHER_NRO}" >&2; exit 1; }
echo "Built the Switch 2 launcher: ${LAUNCHER_NRO}"
sha256sum "${LAUNCHER_NRO}"
