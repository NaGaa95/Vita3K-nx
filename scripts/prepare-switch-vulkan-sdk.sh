#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

SDK_VERSION=26.2.0
SDK_ARCHIVE_SHA256=a1026f1b5348b89b87b0904ae13527485a4f0cb5d58088f9230f9c0b5cdb2154
SDK_NAME="mesa-${SDK_VERSION}-switch-unified-horizon-sdk"
BUILD_ROOT="${ROOT}/build-switch-deps"
ARCHIVE=""
CHECK_ONLY=0

usage() {
	cat <<EOF
Usage: $0 [--check] [--archive ZIP] [--build-root DIR]

Verifies and extracts the pinned Mesa ${SDK_VERSION} unified Switch SDK.
The resulting SWITCH_VULKAN_SDK is:
  <build-root>/${SDK_NAME}

--archive is required for a new extraction and ignored by --check.
EOF
}

while (($#)); do
	case "$1" in
		--check)
			CHECK_ONLY=1
			shift
			;;
		--archive)
			[[ $# -ge 2 ]] || { usage >&2; exit 2; }
			ARCHIVE="$2"
			shift 2
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

SDK_ROOT="${BUILD_ROOT}/${SDK_NAME}"

check_sdk() {
	local sdk_root="$1"
	local vulkan_pc="${sdk_root}/lib/pkgconfig/vulkan.pc"
	local failed=0

	for path in \
		"${sdk_root}/include/vulkan/vulkan.h" \
		"${sdk_root}/include/EGL/egl.h" \
		"${sdk_root}/include/GL/gl.h" \
		"${sdk_root}/lib/libvulkan.a" \
		"${sdk_root}/lib/libnvk.a" \
		"${sdk_root}/lib/libEGL.a" \
		"${sdk_root}/lib/libGL.a" \
		"${sdk_root}/lib/cmake/OpenGL/OpenGLConfig.cmake" \
		"${vulkan_pc}"; do
		if [[ ! -f "${path}" ]]; then
			echo "Missing Mesa Switch SDK artifact: ${path}" >&2
			failed=1
		fi
	done
	((failed == 0)) || return 1

	grep -Fqx "Version: ${SDK_VERSION}" "${vulkan_pc}" || {
		echo "Mesa Switch SDK is not version ${SDK_VERSION}: ${vulkan_pc}" >&2
		return 1
	}
	echo "Ready: unified Mesa Switch SDK ${SDK_VERSION} at ${sdk_root}"
}

if ((CHECK_ONLY)); then
	check_sdk "${SDK_ROOT}"
	exit
fi

if [[ -d "${SDK_ROOT}" ]]; then
	check_sdk "${SDK_ROOT}"
	exit
fi

[[ -n "${ARCHIVE}" ]] || {
	echo "--archive is required because ${SDK_ROOT} does not exist" >&2
	usage >&2
	exit 2
}

for tool in sha256sum mktemp; do
	command -v "${tool}" >/dev/null || {
		echo "Required host tool not found: ${tool}" >&2
		exit 1
	}
done
if command -v unzip >/dev/null; then
	EXTRACT_COMMAND=(unzip -q "${ARCHIVE}" -d)
elif command -v bsdtar >/dev/null; then
	EXTRACT_COMMAND=(bsdtar -xf "${ARCHIVE}" -C)
elif command -v tar >/dev/null; then
	EXTRACT_COMMAND=(tar -xf "${ARCHIVE}" -C)
else
	echo "Required host tool not found: unzip or tar" >&2
	exit 1
fi
[[ -f "${ARCHIVE}" ]] || { echo "Mesa Switch SDK archive not found: ${ARCHIVE}" >&2; exit 1; }

mkdir -p "${BUILD_ROOT}"
printf '%s  %s\n' "${SDK_ARCHIVE_SHA256}" "${ARCHIVE}" | sha256sum --check --strict -

STAGING="$(mktemp -d "${BUILD_ROOT}/.${SDK_NAME}.tmp.XXXXXX")"
cleanup() {
	if [[ -n "${STAGING:-}" && -d "${STAGING}" ]]; then
		rm -rf -- "${STAGING}"
	fi
}
trap cleanup EXIT

"${EXTRACT_COMMAND[@]}" "${STAGING}"
EXTRACTED_ROOT="${STAGING}/opt/devkitpro/portlibs/switch"
if [[ ! -d "${EXTRACTED_ROOT}" ]]; then
	EXTRACTED_ROOT="${STAGING}"
fi
check_sdk "${EXTRACTED_ROOT}"
mv "${EXTRACTED_ROOT}" "${SDK_ROOT}"
rm -rf -- "${STAGING}"
STAGING=""

echo "Set SWITCH_VULKAN_SDK=${SDK_ROOT}"
