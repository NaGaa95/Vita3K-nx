add_library(
	renderer
	STATIC
	src/gl/attribute_formats.cpp
	src/gl/color_formats.cpp
	src/gl/compile_program.cpp
	src/gl/draw.cpp
	src/gl/fence.cpp
	src/gl/renderer.cpp
	src/gl/ring_buffer.cpp
	src/gl/overlay_renderer.cpp
	src/gl/screen_render.cpp
	src/gl/surface_cache.cpp
	src/gl/sync_state.cpp
	src/gl/texture_formats.cpp
	src/gl/texture.cpp
	src/gl/uniforms.cpp

	src/vulkan/allocator.cpp
	src/vulkan/context.cpp
	src/vulkan/creation.cpp
	src/vulkan/gxm_to_vulkan.cpp
	src/vulkan/pipeline_cache.cpp
	src/vulkan/renderer.cpp
	src/vulkan/scene.cpp
	src/vulkan/overlay_renderer.cpp
	src/vulkan/screen_filters.cpp
	src/vulkan/screen_renderer.cpp
	src/vulkan/surface_cache.cpp
	src/vulkan/sync_state.cpp
	src/vulkan/texture.cpp

	src/texture/cache.cpp
	src/texture/format.cpp
	src/texture/palette.cpp
	src/texture/pvrt-dec.cpp
	src/texture/replacement.cpp
	src/texture/yuv.cpp

	src/batch.cpp
	src/creation.cpp
	src/renderer.cpp
	src/scene.cpp
	src/shaders.cpp
	src/state_set.cpp
	src/sync.cpp
	src/transfer.cpp
)

target_include_directories(renderer PUBLIC include)
target_link_libraries(renderer PUBLIC display mem stb shader glutil threads config util vkutil overlay)
target_link_libraries(renderer PRIVATE dialog ddspp SDL3::SDL3 stb ffmpeg xxHash::xxhash concurrentqueue)

if(SWITCH)
	# The unified Horizon SDK provides native NVC0 OpenGL and Zink alongside NVK.
	# The `vulkan` interface already carries OpenGL::GL's complete unified static
	# rescan group. Do not link it again here: NVK, EGL, Nouveau, and Zink must
	# share one archive instance and one Horizon runtime.
	target_sources(renderer PRIVATE src/vulkan/lsfg.cpp)
	set(LSFG_ROOT "${CMAKE_SOURCE_DIR}/external/lsfg-vk")
	add_library(vita3k_switch_lsfg STATIC
		"${LSFG_ROOT}/bridge/lsfg_bridge.cpp"
		"${LSFG_ROOT}/lsfg-vk-common/src/helpers/errors.cpp"
		"${LSFG_ROOT}/lsfg-vk-common/src/vulkan/buffer.cpp"
		"${LSFG_ROOT}/lsfg-vk-common/src/vulkan/command_buffer.cpp"
		"${LSFG_ROOT}/lsfg-vk-common/src/vulkan/descriptor_pool.cpp"
		"${LSFG_ROOT}/lsfg-vk-common/src/vulkan/descriptor_set.cpp"
		"${LSFG_ROOT}/lsfg-vk-common/src/vulkan/fence.cpp"
		"${LSFG_ROOT}/lsfg-vk-common/src/vulkan/image.cpp"
		"${LSFG_ROOT}/lsfg-vk-common/src/vulkan/sampler.cpp"
		"${LSFG_ROOT}/lsfg-vk-common/src/vulkan/semaphore.cpp"
		"${LSFG_ROOT}/lsfg-vk-common/src/vulkan/shader.cpp"
		"${LSFG_ROOT}/lsfg-vk-common/src/vulkan/timeline_semaphore.cpp"
		"${LSFG_ROOT}/lsfg-vk-common/src/vulkan/vulkan.cpp"
		"${LSFG_ROOT}/lsfg-vk-backend/src/extraction/dll_reader.cpp"
		"${LSFG_ROOT}/lsfg-vk-backend/src/extraction/shader_registry.cpp"
		"${LSFG_ROOT}/lsfg-vk-backend/src/helpers/limits.cpp"
		"${LSFG_ROOT}/lsfg-vk-backend/src/helpers/managed_shader.cpp"
		"${LSFG_ROOT}/lsfg-vk-backend/src/helpers/utils.cpp"
		"${LSFG_ROOT}/lsfg-vk-backend/src/lsfgvk.cpp"
		"${LSFG_ROOT}/lsfg-vk-backend/src/shaderchains/alpha0.cpp"
		"${LSFG_ROOT}/lsfg-vk-backend/src/shaderchains/alpha1.cpp"
		"${LSFG_ROOT}/lsfg-vk-backend/src/shaderchains/beta0.cpp"
		"${LSFG_ROOT}/lsfg-vk-backend/src/shaderchains/beta1.cpp"
		"${LSFG_ROOT}/lsfg-vk-backend/src/shaderchains/delta0.cpp"
		"${LSFG_ROOT}/lsfg-vk-backend/src/shaderchains/delta1.cpp"
		"${LSFG_ROOT}/lsfg-vk-backend/src/shaderchains/gamma0.cpp"
		"${LSFG_ROOT}/lsfg-vk-backend/src/shaderchains/gamma1.cpp"
		"${LSFG_ROOT}/lsfg-vk-backend/src/shaderchains/generate.cpp"
		"${LSFG_ROOT}/lsfg-vk-backend/src/shaderchains/mipmaps.cpp"
	)
	target_include_directories(vita3k_switch_lsfg PUBLIC
		"${LSFG_ROOT}/bridge"
		"${LSFG_ROOT}/lsfg-vk-common/include"
		"${LSFG_ROOT}/lsfg-vk-backend/include"
		"${LSFG_ROOT}/lsfg-vk-backend/src"
	)
	target_compile_options(vita3k_switch_lsfg PRIVATE -fexceptions)
	target_compile_definitions(vita3k_switch_lsfg PRIVATE
		USE_VULKAN=1
		VK_NO_PROTOTYPES=1
		LSFG_VITA3K_STATIC_VULKAN_LOADER=1
		# LSFG's common library calls its private C-Vulkan wrapper namespace
		# `vk`, which would violate the ODR beside Vita3K's Vulkan-Hpp `vk`
		# namespace once the Switch executable is merged by LTO.
		vk=lsfg_internal_vk
	)
	target_link_libraries(vita3k_switch_lsfg PRIVATE vulkan)
	target_link_libraries(renderer PRIVATE vita3k_switch_lsfg)
endif()

if(ANDROID)
	target_link_libraries(renderer PRIVATE android adrenotools)
endif()

if(APPLE)
	target_sources(renderer PRIVATE src/vulkan/metal_layer.mm)
	target_link_libraries(renderer PRIVATE "-framework QuartzCore" "-framework Cocoa")
endif()

if(X11_FOUND)
	target_link_libraries(renderer PRIVATE ${X11_LIBRARIES})
	target_include_directories(renderer PRIVATE ${X11_INCLUDE_DIR})
endif()

if(WAYLAND_FOUND)
	target_link_libraries(renderer PRIVATE ${WAYLAND_LIBRARIES})
	target_include_directories(renderer PRIVATE ${WAYLAND_INCLUDE_DIRS})
endif()

if ("${CMAKE_CXX_COMPILER_ID}" MATCHES "Clang")
	# silence all vma warnings
	target_compile_options(renderer PRIVATE "-Wno-nullability-completeness")
endif()

# Marshmallow Tracy linking
if(TRACY_ENABLE_ON_CORE_COMPONENTS)
	target_link_libraries(renderer PRIVATE tracy)
endif()

if(NOT ANDROID AND NOT SWITCH)
    add_executable(renderer-format-tests tests/packed_formats.cpp)
    target_include_directories(renderer-format-tests PRIVATE include)
    add_test(NAME renderer-formats COMMAND renderer-format-tests)
    add_executable(renderer-readback-tests tests/readback_rows.cpp)
    target_include_directories(renderer-readback-tests PRIVATE include)
    add_test(NAME renderer-readback COMMAND renderer-readback-tests)
endif()
