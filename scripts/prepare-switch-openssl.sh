#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

OPENSSL_VERSION=4.0.1
OPENSSL_SHA256=2db3f3a0d6ea4b59e1f094ace2c8cd536dffb87cdc39084c5afa1e6f7f37dd09
OPENSSL_URL="https://github.com/openssl/openssl/releases/download/openssl-${OPENSSL_VERSION}/openssl-${OPENSSL_VERSION}.tar.gz"
BUILD_ROOT="${ROOT}/build-switch-deps"
CHECK_ONLY=0

usage() {
	cat <<EOF
Usage: $0 [--check] [--build-root DIR]

Downloads, verifies, and cross-builds the pinned OpenSSL ${OPENSSL_VERSION}
static libraries for Nintendo Switch. The resulting SWITCH_OPENSSL_DIR is:
  <build-root>/openssl-${OPENSSL_VERSION}
EOF
}

while (($#)); do
	case "$1" in
		--check)
			CHECK_ONLY=1
			shift
			;;
		--build-root)
			[[ $# -ge 2 ]] || { usage >&2; exit 2; }
			BUILD_ROOT="$2"
			shift 2
			;;
		-h|--help)
			usage
			exit 0
			;;
		*)
			echo "Unknown argument: $1" >&2
			usage >&2
			exit 2
			;;
	esac
done

SOURCE_DIR="${BUILD_ROOT}/openssl-${OPENSSL_VERSION}"
ARCHIVE="${BUILD_ROOT}/openssl-${OPENSSL_VERSION}.tar.gz"
COMPAT_DIR="${ROOT}/cmake/switch/openssl-compat"
STAMP="${SOURCE_DIR}/.vita3k-switch-build"

ensure_directory() {
	[[ -d "$1" ]] || mkdir "$1"
}

check_result() {
	local failed=0
	for path in \
		"${SOURCE_DIR}/libcrypto.a" \
		"${SOURCE_DIR}/libssl.a" \
		"${SOURCE_DIR}/include/openssl/ssl.h" \
		"${SOURCE_DIR}/include/openssl/configuration.h"; do
		if [[ ! -f "${path}" ]]; then
			echo "Missing Switch OpenSSL artifact: ${path}" >&2
			failed=1
		fi
	done
	if [[ ! -f "${STAMP}" ]] || ! grep -qx "openssl-${OPENSSL_VERSION} aarch64-none-elf" "${STAMP}"; then
		echo "Missing or stale Switch OpenSSL build stamp: ${STAMP}" >&2
		failed=1
	fi
	((failed == 0)) || return 1
	echo "Ready: OpenSSL ${OPENSSL_VERSION} for aarch64-none-elf at ${SOURCE_DIR}"
}

if ((CHECK_ONLY)); then
	check_result
	exit
fi

: "${DEVKITPRO:=/opt/devkitpro}"
DEVKITA64="${DEVKITA64:-${DEVKITPRO}/devkitA64}"
TOOLCHAIN_BIN="${DEVKITA64}/bin"

for tool in curl perl make tar sha256sum; do
	command -v "${tool}" >/dev/null || { echo "Required host tool not found: ${tool}" >&2; exit 1; }
done
[[ -x "${TOOLCHAIN_BIN}/aarch64-none-elf-gcc" ]] || {
	echo "devkitA64 compiler not found at ${TOOLCHAIN_BIN}/aarch64-none-elf-gcc" >&2
	exit 1
}

ensure_directory "${BUILD_ROOT}"
if [[ ! -f "${ARCHIVE}" ]]; then
	echo "Downloading OpenSSL ${OPENSSL_VERSION}..."
	curl --fail --location --retry 3 --output "${ARCHIVE}.part" "${OPENSSL_URL}"
	mv "${ARCHIVE}.part" "${ARCHIVE}"
fi

printf '%s  %s\n' "${OPENSSL_SHA256}" "${ARCHIVE}" | sha256sum --check --strict -

if [[ ! -f "${SOURCE_DIR}/Configure" ]]; then
	ensure_directory "${SOURCE_DIR}"
	tar -xzf "${ARCHIVE}" -C "${SOURCE_DIR}" --strip-components=1
fi

export PATH="${TOOLCHAIN_BIN}:${PATH}"
export TMPDIR="${BUILD_ROOT}/tmp"
export TEMP="${BUILD_ROOT}/tmp"
export TMP="${BUILD_ROOT}/tmp"
ensure_directory "${TMPDIR}"

cd "${SOURCE_DIR}"

# OpenSSL's unix seed backend probes a weak getentropy symbol. Vita3K supplies
# it with Horizon's csrng service, giving TLS/key generation real OS entropy.
# Providers are built into libcrypto because Horizon has no dlopen modules.
perl ./Configure linux-aarch64 \
	no-shared no-tests no-asm no-apps no-docs no-dso no-quic \
	no-module no-ui-console \
	--cross-compile-prefix=aarch64-none-elf- \
	--with-rand-seed=getrandom \
	-D__SWITCH__ -D_GNU_SOURCE -Dtimezone=_timezone -pipe -fPIE \
	-march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -ftls-model=local-exec \
	-I"${DEVKITPRO}/libnx/include" -I"${COMPAT_DIR}"

JOBS="${VITA3K_BUILD_JOBS:-}"
if [[ -z "${JOBS}" ]]; then
	JOBS="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"
fi
make -j"${JOBS}" build_libs

printf 'openssl-%s aarch64-none-elf\n' "${OPENSSL_VERSION}" >"${STAMP}"
check_result
