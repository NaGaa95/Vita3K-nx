cmake_minimum_required(VERSION 3.22)
project(Vita3KLauncherStorageDependencies LANGUAGES C CXX)

include(FetchContent)

# These archives are consumed by the devkitPro Makefile used for the launcher.
# Keep their code generation deterministic and compatible with that Release
# build rather than inheriting the host CMake configuration.
set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
set(CMAKE_C_FLAGS_RELEASE "-O3 -DNDEBUG" CACHE STRING "" FORCE)
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG" CACHE STRING "" FORCE)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)

set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(ENABLE_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ENABLE_LIBKRB5 OFF CACHE BOOL "" FORCE)
set(ENABLE_GSSAPI OFF CACHE BOOL "" FORCE)

# Pinned Switch port. The upstream NintendoSwitch configuration defines its own
# NEED_READV/NEED_WRITEV shims, while the launcher supplies validated global
# implementations in source/posix_compat.cpp for every dependency that needs
# them. Undefine the private copies to keep one implementation in the final ELF.
# Namespace the AES helper as well because the complete launcher links other
# crypto code.
FetchContent_Declare(libsmb2
    GIT_REPOSITORY https://github.com/ITotalJustice/libsmb2.git
    GIT_TAG 867beea093f2863dfddea01945204f724afd6c45)
FetchContent_MakeAvailable(libsmb2)
target_compile_options(smb2 PRIVATE -UNEED_READV -UNEED_WRITEV)
target_compile_definitions(smb2 PRIVATE AES128_ECB_encrypt=smb2_AES128_ECB_encrypt)

# FAT/exFAT support is sufficient for the import browser. GPL-backed NTFS/ext4
# and the obsolete SX OS backend stay disabled, matching the audited Drastic
# dependency build.
set(USBHSFS_GPL OFF CACHE BOOL "" FORCE)
set(USBHSFS_NTFS OFF CACHE BOOL "" FORCE)
set(USBHSFS_EXT4 OFF CACHE BOOL "" FORCE)
set(USBHSFS_SXOS_DISABLE ON CACHE BOOL "" FORCE)
set(USBHSFS_EXAMPLES OFF CACHE BOOL "" FORCE)

set(_vita3k_libusbhsfs_pinned_revision 625269b7725a6e2a3f2724e8d45b602c1b20ead5)
FetchContent_Declare(libusbhsfs
    GIT_REPOSITORY https://github.com/ITotalJustice/libusbhsfs.git
    GIT_TAG ${_vita3k_libusbhsfs_pinned_revision})
FetchContent_MakeAvailable(libusbhsfs)

# Apply the same audited UASP transport used by Dolphin-NX to the exact pinned
# libusbhsfs revision. The checks make repeated CMake configuration idempotent
# while rejecting a silently changed dependency tree.
find_package(Git REQUIRED)
execute_process(
    COMMAND "${GIT_EXECUTABLE}"
            -c "safe.directory=${libusbhsfs_SOURCE_DIR}"
            rev-parse HEAD
    WORKING_DIRECTORY "${libusbhsfs_SOURCE_DIR}"
    RESULT_VARIABLE _vita3k_libusbhsfs_revision_result
    OUTPUT_VARIABLE _vita3k_libusbhsfs_revision
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
if(NOT _vita3k_libusbhsfs_revision_result EQUAL 0 OR
   NOT "${_vita3k_libusbhsfs_revision}" STREQUAL "${_vita3k_libusbhsfs_pinned_revision}")
    message(FATAL_ERROR
        "libusbhsfs source is not the pinned ${_vita3k_libusbhsfs_pinned_revision} revision")
endif()

set(_vita3k_libusbhsfs_patches
    "${CMAKE_CURRENT_SOURCE_DIR}/libusbhsfs/0001-add-uasp-transport-hooks.patch"
    "${CMAKE_CURRENT_SOURCE_DIR}/libusbhsfs/0002-fallback-to-bot-interface.patch"
    "${CMAKE_CURRENT_SOURCE_DIR}/libusbhsfs/0003-cleanup-after-uasp-reset.patch")
foreach(_vita3k_libusbhsfs_patch IN LISTS _vita3k_libusbhsfs_patches)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}"
                -c "safe.directory=${libusbhsfs_SOURCE_DIR}"
                apply --ignore-space-change --check "${_vita3k_libusbhsfs_patch}"
        WORKING_DIRECTORY "${libusbhsfs_SOURCE_DIR}"
        RESULT_VARIABLE _vita3k_libusbhsfs_patch_check
        OUTPUT_QUIET ERROR_QUIET)
    if(_vita3k_libusbhsfs_patch_check EQUAL 0)
        execute_process(
            COMMAND "${GIT_EXECUTABLE}"
                    -c "safe.directory=${libusbhsfs_SOURCE_DIR}"
                    apply --ignore-space-change "${_vita3k_libusbhsfs_patch}"
            WORKING_DIRECTORY "${libusbhsfs_SOURCE_DIR}"
            RESULT_VARIABLE _vita3k_libusbhsfs_patch_result)
        if(NOT _vita3k_libusbhsfs_patch_result EQUAL 0)
            message(FATAL_ERROR "Failed to apply ${_vita3k_libusbhsfs_patch}")
        endif()
    else()
        execute_process(
            COMMAND "${GIT_EXECUTABLE}"
                    -c "safe.directory=${libusbhsfs_SOURCE_DIR}"
                    apply --ignore-space-change --reverse --check "${_vita3k_libusbhsfs_patch}"
            WORKING_DIRECTORY "${libusbhsfs_SOURCE_DIR}"
            RESULT_VARIABLE _vita3k_libusbhsfs_patch_reverse_check
            OUTPUT_QUIET ERROR_QUIET)
        if(NOT _vita3k_libusbhsfs_patch_reverse_check EQUAL 0)
            message(FATAL_ERROR
                "The pinned libusbhsfs source does not match ${_vita3k_libusbhsfs_patch}")
        endif()
    endif()
endforeach()

target_sources(libusbhsfs PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/libusbhsfs/usbhsfs_uasp.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/libusbhsfs/usbhsfs_uasp.h")
target_include_directories(libusbhsfs PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/libusbhsfs"
    "${libusbhsfs_SOURCE_DIR}/source")

# Export the public headers to a stable build-tree location. FetchContent may
# use its default _deps sources or caller-provided, pre-populated source trees;
# the standalone devkitPro Makefile must not depend on either internal layout.
file(REMOVE_RECURSE "${CMAKE_BINARY_DIR}/include/smb2")
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/include")
file(COPY "${libsmb2_SOURCE_DIR}/include/smb2"
    DESTINATION "${CMAKE_BINARY_DIR}/include")
configure_file("${libusbhsfs_SOURCE_DIR}/include/usbhsfs.h"
    "${CMAKE_BINARY_DIR}/include/usbhsfs.h" COPYONLY)

# Keep libusbhsfs' embedded FatFs timestamp hook private if another FatFs copy
# is linked into the launcher later.
target_compile_definitions(libusbhsfs PRIVATE get_fattime=usbhsfs_get_fattime)

add_library(vita3k_launcher_storage_dependencies INTERFACE)
target_link_libraries(vita3k_launcher_storage_dependencies INTERFACE smb2 libusbhsfs)
add_library(Vita3KLauncher::StorageDependencies ALIAS
    vita3k_launcher_storage_dependencies)
