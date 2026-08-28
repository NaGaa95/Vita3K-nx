# ---------------------------------------------------------------------------
# cmake/switch.cmake
#
# Nintendo Switch (Horizon / libnx) build wiring for Vita3K.
#
# This file is included from the top-level CMakeLists.txt when the devkitPro
# "NintendoSwitch" toolchain is active (invoke CMake through
# aarch64-none-elf-cmake, or pass -DCMAKE_TOOLCHAIN_FILE=<dkp>/cmake/Switch.cmake).
#
# The NintendoSwitch platform file already:
#   * sets CMAKE_SYSTEM_NAME = NintendoSwitch and NINTENDO_SWITCH = TRUE
#   * defines __SWITCH__ and the arch flags
#     (-march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -ftls-model=local-exec)
#   * adds -fPIE -specs=<libnx>/switch.specs to the link flags
#   * puts portlibs/switch + libnx on the find-root path
#   * provides nx_create_nro() / nx_generate_nacp() packaging helpers
#
# This file only adds Vita3K-specific pieces: the unified Mesa NVK, Nouveau,
# and Zink graphics stack, a few global gating switches, and portlib
# include/lib visibility.
# ---------------------------------------------------------------------------

message(STATUS "================================================================")
message(STATUS " Vita3K: configuring for Nintendo Switch (libnx / devkitA64)")
message(STATUS "================================================================")

# DEVKITPRO is exported by the aarch64-none-elf-cmake wrapper; fall back to the
# canonical install path if a bare cmake was used with the toolchain file.
if(DEFINED ENV{DEVKITPRO})
	file(TO_CMAKE_PATH "$ENV{DEVKITPRO}" DEVKITPRO)
elseif(EXISTS "/opt/devkitpro")
	set(DEVKITPRO "/opt/devkitpro")
else()
	message(FATAL_ERROR "DEVKITPRO is not set and /opt/devkitpro does not exist")
endif()

set(SWITCH_PORTLIBS "${DEVKITPRO}/portlibs/switch")
set(LIBNX          "${DEVKITPRO}/libnx")

# Make the portlib + libnx headers/libs visible to plain find_path/find_library
# and to targets that just #include <...> without going through a portlib target.
include_directories(SYSTEM "${SWITCH_PORTLIBS}/include" "${LIBNX}/include")
link_directories("${SWITCH_PORTLIBS}/lib" "${LIBNX}/lib")

# Project-wide Switch identification.
#
# IMPORTANT: __SWITCH__ and the arch flags normally come from the NintendoSwitch
# platform file via CMAKE_CXX_FLAGS, but passing -DCMAKE_CXX_FLAGS=... on the
# command line (e.g. for -pipe) *replaces* that value and silently drops them.
# Re-assert them here so every __SWITCH__ guard works and the correct Cortex-A57
# TLS/crypto codegen is used regardless of how CMAKE_*_FLAGS was set.
add_compile_definitions(__SWITCH__ VITA3K_SWITCH)
add_compile_options(
	-march=armv8-a+crc+crypto
	-mtune=cortex-a57
	-mtp=soft
	-ftls-model=local-exec)

# devkitPro's NintendoSwitch platform uses -O2 for Release builds. Vita3K's
# CPU translation, dispatch, and renderer hot paths benefit from the regular
# CMake GNU Release level used by the Ticu port. Append -O3 so it takes
# precedence over the platform default without replacing the cached toolchain
# flags (which also contain required Horizon options).
add_compile_options("$<$<CONFIG:Release>:-O3>")

# Use pipes instead of temporary files between compiler stages. This both speeds
# up the build and avoids depending on a writable native %TEMP% (some msys2/CI
# environments hand the devkitA64 gcc.exe an unwritable C:\WINDOWS temp dir).
add_compile_options(-pipe)

# newlib hides POSIX/GNU functions (fileno, fdopen, putc_unlocked, fwrite_unlocked,
# strcasestr, ...) behind visibility macros; _GNU_SOURCE exposes them. Needed by
# fmt, spdlog and various Vita3K TUs.
add_compile_definitions(_GNU_SOURCE)

# vulkan.hpp's DynamicLoader (dlopen-based) has no Horizon platform case and is
# unused here (the dispatcher is seeded directly with the statically-linked
# vkGetInstanceProcAddr). Disable it globally — some TUs include vulkan.hpp
# without going through vkutil.h.
add_compile_definitions(VULKAN_HPP_ENABLE_DYNAMIC_LOADER_TOOL=0)

# Precompiled headers break under the msys2-cmake-drives-native-gcc combo (the
# generated PCH wrapper references sources via msys '/c/...' paths the native
# gcc.exe cannot resolve). Disable PCH for the Switch build.
set(CMAKE_DISABLE_PRECOMPILE_HEADERS ON CACHE BOOL "" FORCE)

# newlib's struct tm does not expose the non-standard tm_gmtoff member used by
# spdlog's %z formatter. Keep logging functional and omit only that unsupported
# timezone-offset conversion on Horizon.
set(SPDLOG_NO_TZ_OFFSET ON CACHE BOOL "" FORCE)

# devkitPro does not ship a Switch OpenSSL portlib. Vita3K needs the complete
# crypto/TLS stack for PKG/PFS installation, SceSsl, and network requests, so a
# stub build is not useful. scripts/prepare-switch-openssl.sh produces this
# pinned relocatable source/build directory.
set(SWITCH_OPENSSL_DIR "" CACHE PATH
	"Path to the Vita3K OpenSSL 4.0.1 Switch build (libcrypto.a, libssl.a, include/)")
if(NOT SWITCH_OPENSSL_DIR AND DEFINED ENV{VITA3K_SWITCH_OPENSSL_DIR})
	set(SWITCH_OPENSSL_DIR "$ENV{VITA3K_SWITCH_OPENSSL_DIR}" CACHE PATH
		"Path to the Vita3K OpenSSL 4.0.1 Switch build" FORCE)
endif()
if(NOT SWITCH_OPENSSL_DIR)
	message(FATAL_ERROR
		"SWITCH_OPENSSL_DIR is required. Run scripts/prepare-switch-openssl.sh, then "
		"configure with -DSWITCH_OPENSSL_DIR=<build-switch-deps/openssl-4.0.1>.")
endif()

file(REAL_PATH "${SWITCH_OPENSSL_DIR}" SWITCH_OPENSSL_DIR EXPAND_TILDE)
set(SWITCH_OPENSSL_DIR "${SWITCH_OPENSSL_DIR}" CACHE PATH
	"Path to the Vita3K OpenSSL 4.0.1 Switch build" FORCE)
foreach(_openssl_file
		libcrypto.a
		libssl.a
		include/openssl/ssl.h
		include/openssl/configuration.h
		include/openssl/opensslv.h)
	if(NOT EXISTS "${SWITCH_OPENSSL_DIR}/${_openssl_file}")
		message(FATAL_ERROR
			"Invalid Switch OpenSSL directory: missing ${SWITCH_OPENSSL_DIR}/${_openssl_file}")
	endif()
endforeach()
file(STRINGS "${SWITCH_OPENSSL_DIR}/include/openssl/opensslv.h"
	_switch_openssl_version_line REGEX "^# *define OPENSSL_VERSION_TEXT " LIMIT_COUNT 1)
if(NOT _switch_openssl_version_line MATCHES "OpenSSL 4\\.0\\.1")
	message(FATAL_ERROR
		"Vita3K Switch requires the pinned OpenSSL 4.0.1 build; found: ${_switch_openssl_version_line}")
endif()
message(STATUS "Switch: using OpenSSL 4.0.1 at ${SWITCH_OPENSSL_DIR}")

# SDL's dynamic-API indirection layer is disabled for the Switch via a
# __SWITCH__ case added to external/sdl/src/dynapi/SDL_dynapi.h (SDL refuses a
# command-line -DSDL_DYNAMIC_API and requires editing that header).

# Features that make no sense / are unavailable on Switch.
set(USE_DISCORD_RICH_PRESENCE OFF CACHE BOOL "" FORCE)  # x86_64/APPLE only anyway
# The port is now past early bring-up. Match the Ticu Release configuration and
# let GCC optimize across Vita3K's many static libraries; this particularly
# helps the small CPU/JIT/HLE helpers on the serial guest execution path.
set(USE_LTO RELEASE_ONLY CACHE STRING "" FORCE)

# CMake selects -flto=auto for GCC, but Ninja does not provide GNU make's
# jobserver and devkitA64 consequently runs the LTRANS partitions serially.
# Bound the Release link to eight workers explicitly. This changes build
# parallelism only; the objects remain regular GCC LTO objects.
add_link_options("$<$<CONFIG:Release>:-flto=8>")

# ---------------------------------------------------------------------------
# Unified Mesa NVK/Nouveau/Zink static graphics SDK
#
# The supported input is the relocatable unified Horizon SDK. Its OpenGL CMake
# package preserves the static archive rescan order required by EGL, Nouveau,
# Zink, and NVK.
# ---------------------------------------------------------------------------
set(SWITCH_VULKAN_SDK "" CACHE PATH
	"Path to the extracted unified Mesa Switch SDK (contains include/, lib/, and lib/cmake/OpenGL/)")

if(NOT SWITCH_VULKAN_SDK AND DEFINED ENV{VITA3K_SWITCH_VULKAN_SDK})
	set(SWITCH_VULKAN_SDK "$ENV{VITA3K_SWITCH_VULKAN_SDK}" CACHE PATH
		"Path to the extracted Mesa NVK Switch Vulkan SDK" FORCE)
endif()

# Kept only to make an old local cache fail with a useful migration message.
set(SWITCH_MESA_DIR "" CACHE PATH
	"Deprecated: use SWITCH_VULKAN_SDK with the packaged Mesa Switch Vulkan SDK")
if(SWITCH_MESA_DIR AND NOT SWITCH_VULKAN_SDK)
	message(FATAL_ERROR
		"SWITCH_MESA_DIR points at Mesa's old build-tree layout, which is no longer "
		"supported. Extract mesa-26.2.0-switch-unified-horizon-sdk.zip and pass "
		"-DSWITCH_VULKAN_SDK=<extracted-sdk-root> instead.")
endif()

set(SWITCH_VULKAN_LINK_LIBS "" CACHE INTERNAL "Mesa NVK driver link line" FORCE)
set(SWITCH_VULKAN_INCLUDE_DIR "" CACHE INTERNAL "Mesa NVK SDK include directory" FORCE)
set(SWITCH_VULKAN_LINK_DEPENDS "" CACHE INTERNAL "Mesa NVK SDK relink inputs" FORCE)

if(NOT SWITCH_VULKAN_SDK)
	message(FATAL_ERROR
		"SWITCH_VULKAN_SDK is required. Extract the Mesa 26.2.0 unified Switch SDK "
		"and configure with -DSWITCH_VULKAN_SDK=<extracted-sdk-root>.")
endif()

file(REAL_PATH "${SWITCH_VULKAN_SDK}" SWITCH_VULKAN_SDK EXPAND_TILDE)
set(SWITCH_VULKAN_SDK "${SWITCH_VULKAN_SDK}" CACHE PATH
	"Path to the extracted Mesa NVK Switch Vulkan SDK" FORCE)

set(_switch_vulkan_required_files
	"include/vulkan/vulkan.h"
	"include/EGL/egl.h"
	"include/GL/gl.h"
	"lib/libvulkan.a"
	"lib/libEGL.a"
	"lib/libGL.a"
	"lib/cmake/OpenGL/OpenGLConfig.cmake")
foreach(_file IN LISTS _switch_vulkan_required_files)
	if(NOT EXISTS "${SWITCH_VULKAN_SDK}/${_file}")
		message(FATAL_ERROR
			"Invalid Mesa NVK Switch SDK: missing ${SWITCH_VULKAN_SDK}/${_file}")
	endif()
endforeach()

set(_switch_vulkan_required_portlibs
	libelf.a
	libexpat.a
	libdrm_nouveau.a
	libzstd.a
	libz.a)
foreach(_library IN LISTS _switch_vulkan_required_portlibs)
	if(NOT EXISTS "${SWITCH_PORTLIBS}/lib/${_library}")
		message(FATAL_ERROR
			"Mesa NVK requires the devkitPro Switch portlib ${_library}, but it was not found at "
			"${SWITCH_PORTLIBS}/lib/${_library}")
	endif()
endforeach()

if(NOT EXISTS "${SWITCH_VULKAN_SDK}/lib/pkgconfig/vulkan.pc")
	message(FATAL_ERROR
		"Invalid unified Mesa Switch SDK: missing lib/pkgconfig/vulkan.pc")
endif()
file(STRINGS "${SWITCH_VULKAN_SDK}/lib/pkgconfig/vulkan.pc"
	_switch_vulkan_version_line REGEX "^Version: " LIMIT_COUNT 1)
string(REGEX REPLACE "^Version: *" "" SWITCH_VULKAN_SDK_VERSION
	"${_switch_vulkan_version_line}")
set(SWITCH_VULKAN_SDK_REVISION "unified")
if(NOT SWITCH_VULKAN_SDK_VERSION)
	message(FATAL_ERROR "Mesa Switch SDK metadata does not contain a Mesa version")
elseif(SWITCH_VULKAN_SDK_VERSION VERSION_LESS "26.2.0")
	message(FATAL_ERROR
		"Mesa NVK ${SWITCH_VULKAN_SDK_VERSION} is too old; Vita3K Switch requires 26.2.0 or newer")
endif()
set(SWITCH_VULKAN_INCLUDE_DIR "${SWITCH_VULKAN_SDK}/include" CACHE INTERNAL
	"Mesa NVK SDK include directory" FORCE)

# Prelink the native ICD exactly like the working Switch ports. Mesa's unified
# archive contains full and Zink-lite Vulkan instance implementations with the
# same internal symbol names; a normal consumer link can otherwise select the
# lite implementation before native NVK creates its instance.
set(SWITCH_MESA_LOCAL_DIR "${CMAKE_BINARY_DIR}/mesa-vulkan-local")
set(SWITCH_MESA_LOCAL_OBJECT "${SWITCH_MESA_LOCAL_DIR}/libvulkan_local.o")
set(SWITCH_MESA_LOCAL_ARCHIVE "${SWITCH_MESA_LOCAL_DIR}/libvulkan_local.a")
add_custom_command(
	OUTPUT "${SWITCH_MESA_LOCAL_OBJECT}" "${SWITCH_MESA_LOCAL_ARCHIVE}"
	COMMAND "${CMAKE_COMMAND}" -E make_directory "${SWITCH_MESA_LOCAL_DIR}"
	COMMAND bash "${CMAKE_SOURCE_DIR}/vita3k/switch/localize_mesa_vulkan.sh"
		"${SWITCH_VULKAN_SDK}" "${SWITCH_MESA_LOCAL_OBJECT}"
		"${SWITCH_MESA_LOCAL_ARCHIVE}"
	DEPENDS
		"${SWITCH_VULKAN_SDK}/lib/libvulkan.a"
		"${CMAKE_SOURCE_DIR}/vita3k/switch/localize_mesa_vulkan.sh"
	VERBATIM)
add_custom_target(vita3k_mesa_vulkan_local DEPENDS
	"${SWITCH_MESA_LOCAL_OBJECT}" "${SWITCH_MESA_LOCAL_ARCHIVE}")

# EGL/Zink rescans the localized archive, while native Vita3K links the object
# first. Both paths therefore share one Mesa/Nouveau lifetime graph.
set(OPENGL_SWITCH_VULKAN_LIBRARY "${SWITCH_MESA_LOCAL_ARCHIVE}")
find_package(OpenGL CONFIG REQUIRED
	PATHS "${SWITCH_VULKAN_SDK}/lib/cmake/OpenGL"
	NO_DEFAULT_PATH)

set(SWITCH_VULKAN_LINK_LIBS
	"${SWITCH_MESA_LOCAL_OBJECT}"
	OpenGL::GL
	CACHE INTERNAL "Unified Mesa graphics link target" FORCE)

# Track every packaged archive so replacing either SDK layout reliably relinks.
file(GLOB _switch_vulkan_archives CONFIGURE_DEPENDS "${SWITCH_VULKAN_SDK}/lib/*.a")
set(SWITCH_VULKAN_LINK_DEPENDS ${_switch_vulkan_archives}
	"${SWITCH_MESA_LOCAL_OBJECT}" "${SWITCH_MESA_LOCAL_ARCHIVE}" CACHE INTERNAL
	"Mesa NVK SDK relink inputs" FORCE)

message(STATUS
	"Switch: unified Mesa SDK ${SWITCH_VULKAN_SDK_VERSION} (${SWITCH_VULKAN_SDK_REVISION}) at ${SWITCH_VULKAN_SDK}")
