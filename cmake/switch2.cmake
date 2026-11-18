# ---------------------------------------------------------------------------
# cmake/switch2.cmake
#
# Nintendo Switch 2 (kaiHEN homebrew) part of the Switch build. cmake/switch.cmake
# includes it in place of its Mesa section when the toolchain is ouncebrew's
# $DEVKITPRO/libounce/cmake/Switch2.cmake, which sets NINTENDO_SWITCH2, defines
# __SWITCH2__ and builds for the Cortex-A78C with libounce (the libnx API) in
# place of libnx. scripts/build-switch2.sh configures it.
#
# Everything that differs from the Switch build is behind NINTENDO_SWITCH2 here
# and __SWITCH2__ in the sources:
#   * The Switch 2 Mesa SDK (mesa-26.2.7-switch2-horizon-sdk.zip, NVK + EGL/GL
#     with nvc0 and Zink). Its libEGL.a and libvulkan.a are GNU ld linker
#     scripts naming the real archives beside them, so its lib directory is
#     searched before every other one (OunceVK's libEGL.a/libvulkan.a in
#     portlibs/switch2 must never be picked).
#   * nx2_platform (vita3k/switch/nx2), the Switch 2 helpers shared with the
#     other NX ports: cores, display, kaiHEN's JIT publishing and the exit
#     cleanup.
#   * No drako3d renderer: Vita3K renders with NVK (Vulkan) or Mesa's GL.
# ---------------------------------------------------------------------------

set(SWITCH2 1)

# nx2_platform's header is visible to every target (dynarmic's code block, the
# CPU, memory and renderer code); the vita3k executable compiles nx2_platform.c
# (vita3k/CMakeLists.txt, with the link options nx2_platform needs).
set(VITA3K_NX2_PLATFORM_DIR "${CMAKE_SOURCE_DIR}/vita3k/switch/nx2")
include_directories("${VITA3K_NX2_PLATFORM_DIR}")

# ---------------------------------------------------------------------------
# Switch 2 Mesa SDK (NVK, EGL, GL with nvc0 and Zink)
# ---------------------------------------------------------------------------
set(SWITCH2_MESA_SDK "" CACHE PATH
	"portlibs/switch2 prefix of the extracted mesa-26.2.7-switch2-horizon-sdk.zip (scripts/prepare-switch2-mesa-sdk.sh)")
if(NOT SWITCH2_MESA_SDK AND DEFINED ENV{VITA3K_SWITCH2_MESA_SDK})
	set(SWITCH2_MESA_SDK "$ENV{VITA3K_SWITCH2_MESA_SDK}" CACHE PATH
		"portlibs/switch2 prefix of the extracted Switch 2 Mesa SDK" FORCE)
endif()
if(NOT SWITCH2_MESA_SDK)
	message(FATAL_ERROR
		"SWITCH2_MESA_SDK is required. Run scripts/prepare-switch2-mesa-sdk.sh --archive "
		"<mesa-26.2.7-switch2-horizon-sdk.zip> and configure with -DSWITCH2_MESA_SDK=<its output>.")
endif()
file(REAL_PATH "${SWITCH2_MESA_SDK}" SWITCH2_MESA_SDK EXPAND_TILDE)
set(SWITCH2_MESA_SDK "${SWITCH2_MESA_SDK}" CACHE PATH
	"portlibs/switch2 prefix of the extracted Switch 2 Mesa SDK" FORCE)

foreach(_file IN ITEMS
		include/vulkan/vulkan.h
		include/EGL/egl.h
		include/GL/gl.h
		lib/libEGL.a
		lib/libvulkan.a
		lib/libmesa_egl.a
		lib/libmesa_gl.a
		lib/libmesa_glapi.a
		lib/libmesa_vulkan.a
		lib/cmake/OpenGL/OpenGLConfig.cmake
		lib/cmake/Vulkan/VulkanConfig.cmake)
	if(NOT EXISTS "${SWITCH2_MESA_SDK}/${_file}")
		message(FATAL_ERROR "Invalid Switch 2 Mesa SDK: missing ${SWITCH2_MESA_SDK}/${_file}")
	endif()
endforeach()
# The SDK's libEGL.a is a linker script naming its own archives; OunceVK's names
# OunceVK's. Refuse anything else (a Switch SDK, or portlibs/switch2 itself).
file(READ "${SWITCH2_MESA_SDK}/lib/libEGL.a" _switch2_egl_script LIMIT 256)
if(NOT _switch2_egl_script MATCHES "mesa_egl" OR NOT _switch2_egl_script MATCHES "mesa_vulkan")
	message(FATAL_ERROR
		"${SWITCH2_MESA_SDK}/lib/libEGL.a is not the Switch 2 Mesa SDK's linker script")
endif()
file(STRINGS "${SWITCH2_MESA_SDK}/lib/cmake/Vulkan/VulkanConfig.cmake" _switch2_vulkan_version
	REGEX "set\\(Vulkan_VERSION " LIMIT_COUNT 1)
string(REGEX REPLACE ".*Vulkan_VERSION \"([0-9.]+)\".*" "\\1" SWITCH2_MESA_SDK_VERSION "${_switch2_vulkan_version}")
file(READ "${SWITCH2_MESA_SDK}/lib/cmake/Vulkan/VulkanConfig.cmake" _switch2_vulkan_config)
if(NOT _switch2_vulkan_config MATCHES "Switch 2")
	message(FATAL_ERROR "${SWITCH2_MESA_SDK} is not the Switch 2 Mesa SDK (VulkanConfig.cmake)")
endif()
if(NOT SWITCH2_MESA_SDK_VERSION OR SWITCH2_MESA_SDK_VERSION VERSION_LESS "26.2.7")
	message(FATAL_ERROR
		"The Switch 2 Mesa SDK is '${SWITCH2_MESA_SDK_VERSION}'; Vita3K requires 26.2.7 or newer")
endif()
# The libraries the SDK's linker scripts name after Mesa, from the Switch
# portlibs (C library only).
foreach(_library IN ITEMS libexpat.a libzstd.a libz.a)
	if(NOT EXISTS "${SWITCH_PORTLIBS}/lib/${_library}")
		message(FATAL_ERROR
			"The Switch 2 Mesa SDK needs the devkitPro Switch portlib ${_library} (${SWITCH_PORTLIBS}/lib)")
	endif()
endforeach()

# -lmesa_egl, -lmesa_vulkan... (the linker scripts) are found in the SDK first,
# then zstd, expat and zlib in the Switch portlibs.
link_directories(BEFORE "${SWITCH2_MESA_SDK}/lib")
# The SDK's EGL/GL/Vulkan headers before the Switch portlibs' (old Mesa) and
# portlibs/switch2's (OunceVK).
include_directories(BEFORE SYSTEM "${SWITCH2_MESA_SDK}/include")

set(SWITCH_VULKAN_INCLUDE_DIR "${SWITCH2_MESA_SDK}/include" CACHE INTERNAL
	"Mesa NVK SDK include directory" FORCE)

# A cached library path would survive a change of SDK and link one Mesa
# generation against another.
get_cmake_property(_switch_cache_variables CACHE_VARIABLES)
foreach(_variable IN LISTS _switch_cache_variables)
	if(_variable MATCHES "^(OPENGL|_OPENGL)_.+_(LIBRARY|INCLUDE_DIR)$")
		unset(${_variable} CACHE)
		unset(${_variable})
	endif()
endforeach()
set(OpenGL_DIR "${SWITCH2_MESA_SDK}/lib/cmake/OpenGL" CACHE PATH
	"OpenGL package from the selected Switch 2 Mesa SDK" FORCE)
find_package(OpenGL CONFIG REQUIRED
	PATHS "${SWITCH2_MESA_SDK}/lib/cmake/OpenGL"
	NO_DEFAULT_PATH)
if(NOT OPENGL_egl_LIBRARY STREQUAL "${SWITCH2_MESA_SDK}/lib/libEGL.a")
	message(FATAL_ERROR "OpenGL::GL resolved to ${OPENGL_egl_LIBRARY}, not the Switch 2 Mesa SDK's libEGL.a")
endif()

# OpenGL::GL is the SDK's libEGL.a script: EGL, the GL frontend and Gallium
# drivers, the GL dispatch and NVK, in the order the SDK links them. It carries
# NVK (vk_icdGetInstanceProcAddr) too, so Vulkan needs nothing more. Vita3K
# reaches Vulkan only through vk_icdGetInstanceProcAddr and the Vulkan-Hpp
# dispatcher, so NVK's public vk* entry points stay as they are.
set(SWITCH_VULKAN_LINK_LIBS OpenGL::GL CACHE INTERNAL "Switch 2 Mesa link target" FORCE)

file(GLOB _switch2_mesa_archives CONFIGURE_DEPENDS "${SWITCH2_MESA_SDK}/lib/*.a")
set(SWITCH_VULKAN_LINK_DEPENDS ${_switch2_mesa_archives} CACHE INTERNAL
	"Switch 2 Mesa SDK relink inputs" FORCE)

message(STATUS "Switch 2: Mesa SDK ${SWITCH2_MESA_SDK_VERSION} at ${SWITCH2_MESA_SDK}")
