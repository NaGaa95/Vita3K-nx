#!/usr/bin/env bash
set -euo pipefail

SDK_ROOT="$1"
OUTPUT_OBJECT="$2"
OUTPUT_ARCHIVE="$3"
DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
BIN="${DEVKITPRO}/devkitA64/bin"
LD="${BIN}/aarch64-none-elf-ld"
NM="${BIN}/aarch64-none-elf-nm"
AR="${BIN}/aarch64-none-elf-ar"
INPUT_ARCHIVE="${SDK_ROOT}/lib/libvulkan.a"
TEMP_MERGED="${OUTPUT_OBJECT}.merged.tmp"
TEMP_ARCHIVE="${OUTPUT_ARCHIVE}.tmp"

cleanup() {
	rm -f "${TEMP_MERGED}" "${TEMP_ARCHIVE}"
}
trap cleanup EXIT

"${LD}" -r \
	-u vk_icdGetInstanceProcAddr \
	-u vk_icdNegotiateLoaderICDInterfaceVersion \
	-u vk_icdGetPhysicalDeviceProcAddr \
	-u nvk_loaderless_GetInstanceProcAddr \
	-u nvk_loaderless_GetDeviceProcAddr \
	--start-group "${INPUT_ARCHIVE}" --end-group \
	-o "${TEMP_MERGED}"

for symbol in \
	vk_icdGetInstanceProcAddr \
	vk_icdNegotiateLoaderICDInterfaceVersion \
	vk_icdGetPhysicalDeviceProcAddr \
	nvk_loaderless_GetInstanceProcAddr \
	nvk_loaderless_GetDeviceProcAddr \
	nouveau_horizon_runtime_get \
	vkQueuePresentKHR \
	posix_memalign; do
	"${NM}" -g --defined-only "${TEMP_MERGED}" | \
		awk -v expected="${symbol}" '$NF == expected { found = 1 } END { exit !found }'
done

mv -f "${TEMP_MERGED}" "${OUTPUT_OBJECT}"
"${AR}" rcs "${TEMP_ARCHIVE}" "${OUTPUT_OBJECT}"
mv -f "${TEMP_ARCHIVE}" "${OUTPUT_ARCHIVE}"
