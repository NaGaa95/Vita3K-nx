#!/usr/bin/env bash
# Verifies and extracts the pinned Mesa Switch 2 SDK (NVK, EGL and GL with nvc0
# and Zink) for the Nintendo Switch 2 build. Prints the resulting
# SWITCH2_MESA_SDK (the zip's opt/devkitpro/portlibs/switch2 prefix).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

SDK_VERSION=26.2.7
# mesa-nx's build-switch2.sh (dist/mesa-26.2.7-switch2-horizon-sdk.zip).
SDK_ARCHIVE_SHA256=e5cf87483ff242b1082e27af5a49fadc71279c55a5f0663986b8b4d76d404971
SDK_NAME="mesa-${SDK_VERSION}-switch2-horizon-sdk"
BUILD_ROOT="${ROOT}/build-switch2-deps"
ARCHIVE=""
CHECK_ONLY=0

usage() {
	cat <<EOF
Usage: $0 [--check] [--archive ZIP] [--build-root DIR]

Verifies and extracts the pinned ${SDK_NAME}.zip.
The resulting SWITCH2_MESA_SDK is:
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
STAMP_NAME=".vita3k-switch2-sdk-sha256"

check_sdk() {
	local sdk_root="$1"
	local failed=0

	for path in \
		"${sdk_root}/include/vulkan/vulkan.h" \
		"${sdk_root}/include/EGL/egl.h" \
		"${sdk_root}/include/GL/gl.h" \
		"${sdk_root}/lib/libEGL.a" \
		"${sdk_root}/lib/libvulkan.a" \
		"${sdk_root}/lib/libmesa_egl.a" \
		"${sdk_root}/lib/libmesa_gl.a" \
		"${sdk_root}/lib/libmesa_glapi.a" \
		"${sdk_root}/lib/libmesa_vulkan.a" \
		"${sdk_root}/lib/cmake/OpenGL/OpenGLConfig.cmake" \
		"${sdk_root}/lib/cmake/Vulkan/VulkanConfig.cmake"; do
		if [[ ! -f "${path}" ]]; then
			echo "Missing Switch 2 Mesa SDK artifact: ${path}" >&2
			failed=1
		fi
	done
	((failed == 0)) || return 1

	# libEGL.a must be the SDK's linker script, never OunceVK's.
	grep -q "mesa_vulkan" "${sdk_root}/lib/libEGL.a" || {
		echo "${sdk_root}/lib/libEGL.a is not the Switch 2 Mesa SDK's linker script" >&2
		return 1
	}
	grep -Fq "Vulkan_VERSION \"${SDK_VERSION}\"" "${sdk_root}/lib/cmake/Vulkan/VulkanConfig.cmake" || {
		echo "The Switch 2 Mesa SDK at ${sdk_root} is not version ${SDK_VERSION}" >&2
		return 1
	}
	grep -Fq "Switch 2" "${sdk_root}/lib/cmake/Vulkan/VulkanConfig.cmake" || {
		echo "${sdk_root} is not the Switch 2 Mesa SDK" >&2
		return 1
	}
	if [[ ! -f "${sdk_root}/${STAMP_NAME}" ]] || [[ "$(<"${sdk_root}/${STAMP_NAME}")" != "${SDK_ARCHIVE_SHA256}" ]]; then
		echo "The Switch 2 Mesa SDK at ${sdk_root} was not extracted from the pinned archive" >&2
		return 1
	fi
}

if ((CHECK_ONLY)); then
	check_sdk "${SDK_ROOT}"
	echo "${SDK_ROOT}"
	exit
fi

if [[ -d "${SDK_ROOT}" ]] && check_sdk "${SDK_ROOT}" 2>/dev/null; then
	echo "${SDK_ROOT}"
	exit
fi

[[ -n "${ARCHIVE}" ]] || {
	echo "--archive is required because ${SDK_ROOT} is missing or stale" >&2
	usage >&2
	exit 2
}
[[ -f "${ARCHIVE}" ]] || { echo "Switch 2 Mesa SDK archive not found: ${ARCHIVE}" >&2; exit 1; }

for tool in sha256sum mktemp; do
	command -v "${tool}" >/dev/null || { echo "Required host tool not found: ${tool}" >&2; exit 1; }
done
if command -v bsdtar >/dev/null; then
	EXTRACT_COMMAND=(bsdtar -xf "${ARCHIVE}" -C)
elif command -v unzip >/dev/null; then
	EXTRACT_COMMAND=(unzip -q "${ARCHIVE}" -d)
else
	echo "Required host tool not found: bsdtar or unzip" >&2
	exit 1
fi

mkdir -p "${BUILD_ROOT}"
printf '%s  %s\n' "${SDK_ARCHIVE_SHA256}" "${ARCHIVE}" | sha256sum --check --strict --quiet - >&2

STAGING="$(mktemp -d "${BUILD_ROOT}/.${SDK_NAME}.tmp.XXXXXX")"
cleanup() {
	if [[ -n "${STAGING:-}" && -d "${STAGING}" ]]; then
		rm -rf -- "${STAGING}"
	fi
}
trap cleanup EXIT

"${EXTRACT_COMMAND[@]}" "${STAGING}" >&2
EXTRACTED_ROOT="${STAGING}/opt/devkitpro/portlibs/switch2"
[[ -d "${EXTRACTED_ROOT}" ]] || {
	echo "The archive has no opt/devkitpro/portlibs/switch2 prefix: ${ARCHIVE}" >&2
	exit 1
}
printf '%s\n' "${SDK_ARCHIVE_SHA256}" > "${EXTRACTED_ROOT}/${STAMP_NAME}"
check_sdk "${EXTRACTED_ROOT}"

if [[ -e "${SDK_ROOT}" ]]; then
	case "$(realpath -m -- "${SDK_ROOT}")" in
		"$(realpath -m -- "${BUILD_ROOT}")"/*) rm -rf -- "${SDK_ROOT}" ;;
		*) echo "Refusing to replace ${SDK_ROOT}" >&2; exit 1 ;;
	esac
fi
mv "${EXTRACTED_ROOT}" "${SDK_ROOT}"
rm -rf -- "${STAGING}"
STAGING=""

echo "${SDK_ROOT}"
