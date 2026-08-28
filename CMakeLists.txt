cmake_minimum_required(VERSION 3.22)

if(CMAKE_SYSTEM_NAME STREQUAL "Android")
	set(ANDROID 1)
	set(VCPKG_TARGET_ANDROID 1)
	# this file must be included before everything else
	include("cmake/vcpkg_android.cmake")
endif()

# Nintendo Switch (Horizon / libnx) homebrew port is built with the devkitPro
# devkitA64 toolchain: pass -DCMAKE_TOOLCHAIN_FILE=<dkp>/cmake/Switch.cmake, which
# sets CMAKE_SYSTEM_NAME=NintendoSwitch, defines __SWITCH__ and the arch flags,
# and adds the switch portlibs to the find-root path. The SWITCH flag itself is
# set just after project() below (the platform vars aren't available until then).

project(Vita3K)

# Detects the amount of processors of the host machine and forwards the result to CPU_COUNT
include(ProcessorCount)
ProcessorCount(CPU_COUNT)

# Define the Architecture variable, right now it should only contain "x86_64" or "arm64"
include("external/dynarmic/CMakeModules/DetectArchitecture.cmake")

# Nintendo Switch detection. The NintendoSwitch platform file (loaded during
# project() above) sets CMAKE_SYSTEM_NAME/NINTENDO_SWITCH, so this must run after
# project(), not before it. The dependency wiring must run before get_boost()/
# add_subdirectory(external).
if(CMAKE_SYSTEM_NAME STREQUAL "NintendoSwitch" OR NINTENDO_SWITCH)
	set(SWITCH 1)
	include("${CMAKE_SOURCE_DIR}/cmake/switch.cmake")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_OSX_DEPLOYMENT_TARGET 13.3)

# workaround for SDL due to cmake bug
# https://github.com/libsdl-org/SDL/issues/6454
# https://gitlab.kitware.com/cmake/cmake/-/issues/24104
if(APPLE)
    enable_language(OBJC)
endif()

if(ANDROID)
	set(CMAKE_SYSROOT "")
	set(CMAKE_OSX_SYSROOT "")
	set_property(GLOBAL
	PROPERTY
	NO_SYSTEM_FROM_IMPORTED ON
	)

	string(APPEND CMAKE_CXX_FLAGS " -Wl,-Bsymbolic -Wno-unused-command-line-argument")
	string(APPEND CMAKE_C_FLAGS " -Wl,-Bsymbolic -Wno-unused-command-line-argument")

	if(CMAKE_BUILD_TYPE STREQUAL "Release")
		set(CMAKE_C_VISIBILITY_PRESET hidden)
		set(CMAKE_CXX_VISIBILITY_PRESET hidden)
		set(CMAKE_VISIBILITY_INLINES_HIDDEN 1)
	endif()
else()
	if((ARCHITECTURE STREQUAL "x86_64") OR APPLE)
		option(USE_DISCORD_RICH_PRESENCE "Build Vita3K with Discord Rich Presence" ON)
	endif()
	option(BUILD_APPIMAGE "Build an AppImage." OFF)
endif()

option(FORCE_BUILD_OPENSSL_MAC OFF)

if("${CMAKE_CXX_COMPILER_LAUNCHER}" STREQUAL "")
	find_program(CCACHE_PROGRAM ccache)
	if(CCACHE_PROGRAM)
		set(CMAKE_C_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
		set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
	endif()
endif()

if(MSVC)
	string(REPLACE "/Zi" "/Z7" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
	string(REPLACE "/Zi" "/Z7" CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG}")
	string(REPLACE "/Zi" "/Z7" CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE}")
	string(REPLACE "/Zi" "/Z7" CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE}")
	string(REPLACE "/Zi" "/Z7" CMAKE_CXX_FLAGS_RELWITHDEBINFO "${CMAKE_CXX_FLAGS_RELWITHDEBINFO}")
	string(REPLACE "/Zi" "/Z7" CMAKE_C_FLAGS_RELWITHDEBINFO "${CMAKE_C_FLAGS_RELWITHDEBINFO}")
endif()

enable_testing()

cmake_policy(SET CMP0069 NEW)
set(CMAKE_POLICY_DEFAULT_CMP0069 NEW)

set(lto_usage_mode ALWAYS RELEASE_ONLY NEVER)
set(USE_LTO RELEASE_ONLY CACHE STRING "Use interprocedural optimization/link time optimization")
set_property(CACHE USE_LTO PROPERTY STRINGS ${lto_usage_mode})

if (NOT (USE_LTO STREQUAL "NEVER"))
	include(CheckIPOSupported)
	if(ANDROID)
		# https://gitlab.kitware.com/cmake/cmake/-/issues/21772
		STRING(REPLACE "-fuse-ld=gold" "" CMAKE_CXX_LINK_OPTIONS_IPO "${CMAKE_CXX_LINK_OPTIONS_IPO}")
		STRING(REPLACE "-fuse-ld=gold" "" CMAKE_C_LINK_OPTIONS_IPO "${CMAKE_C_LINK_OPTIONS_IPO}")
		set(ipo_supported TRUE)
	else()
		check_ipo_supported(RESULT ipo_supported OUTPUT ipo_supported_error)
	endif()

	if( ipo_supported )
		if (USE_LTO STREQUAL "ALWAYS")
			set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
		elseif (USE_LTO STREQUAL "RELEASE_ONLY")
			set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
		endif()
	else()
		message(STATUS "IPO / LTO not supported: <${ipo_supported_error}>")
		set(USE_LTO NEVER CACHE STRING "Use interprocedural optimization/link time optimization")
	endif()
endif()


############################
########## Boost ###########
############################

# Macro to build Boost buildsystem executable
macro(b2_build)
	if (CMAKE_HOST_SYSTEM_NAME STREQUAL "Windows")
		execute_process(
			COMMAND ${BOOST_SOURCEDIR}/bootstrap.bat --with-toolset=${BOOST_TOOLSET}
			WORKING_DIRECTORY ${BOOST_SOURCEDIR}
			OUTPUT_QUIET
		)
	elseif(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
		execute_process(
			COMMAND chmod +x ${BOOST_SOURCEDIR}/tools/build/src/engine/build.sh
			COMMAND sh ${BOOST_SOURCEDIR}/bootstrap.sh
			WORKING_DIRECTORY ${BOOST_SOURCEDIR}
			OUTPUT_QUIET
		)
	elseif(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
		execute_process(
			COMMAND chmod +x ${BOOST_SOURCEDIR}/tools/build/src/engine/build.sh
			COMMAND sh ${BOOST_SOURCEDIR}/bootstrap.sh --with-toolset=${BOOST_TOOLSET}
			WORKING_DIRECTORY ${BOOST_SOURCEDIR}
			OUTPUT_QUIET
		)
	endif()
endmacro(b2_build)

# Macro to compile Boost
macro(boost_compile)
	list(TRANSFORM BOOST_MODULES_TO_FIND PREPEND --with- OUTPUT_VARIABLE b2_cmd_line_common)
	set(b2_cmd_line_common ${b2} ${b2_cmd_line_common} -j${CPU_COUNT} --build-dir=${BOOST_INSTALLDIR} --stagedir=${BOOST_INSTALLDIR} cxxflags=${BOOST_CXX_FLAGS})
	set(Boost_USE_DEBUG_RUNTIME "")
	if (CMAKE_SYSTEM_NAME STREQUAL "Windows")
		execute_process(
			COMMAND ${b2_cmd_line_common} address-model=64 --architecture=x64 toolset=${BOOST_TOOLSET} variant=debug,release stage
			WORKING_DIRECTORY ${BOOST_SOURCEDIR}
			OUTPUT_QUIET
		)
	elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
		execute_process(
			COMMAND ${b2_cmd_line_common} architecture=${BOOST_ARCHITECTURE} cxxflags="-mmacosx-version-min=11.0" stage
			WORKING_DIRECTORY ${BOOST_SOURCEDIR}
			OUTPUT_QUIET
		)
	elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
		execute_process(
			COMMAND ${b2_cmd_line_common} --ignore-site-config toolset=${BOOST_TOOLSET} stage
			WORKING_DIRECTORY ${BOOST_SOURCEDIR}
			OUTPUT_QUIET
		)
	endif()
endmacro(boost_compile)

option(VITA3K_FORCE_CUSTOM_BOOST "Force Vita3K build process to use the Boost version included with the repository" OFF)
option(VITA3K_FORCE_SYSTEM_BOOST "Force Vita3K build process to use the Boost version in the system and CMake's default paths and ignore the Boost version included with Vita3K" OFF)

# Boost modules to be found by CMake
set(BOOST_MODULES_TO_FIND filesystem)

# If build process isn't set to forcefully use system Boost
macro(get_boost)
	if(SWITCH)
		# dkp-libs no longer publishes a Switch Boost package. Build only the one
		# compiled component Vita3K uses from the already-pinned Boost submodule;
		# all of its other Boost dependencies are headers in the unified boost/ tree.
		set(BOOST_SUPERPROJECT_VERSION "1.89.0")
		if(NOT TARGET Boost::filesystem)
			set(_boost_filesystem_source "${CMAKE_SOURCE_DIR}/external/boost/libs/filesystem/src")
			add_library(boost_filesystem STATIC
				"${_boost_filesystem_source}/codecvt_error_category.cpp"
				"${_boost_filesystem_source}/exception.cpp"
				"${_boost_filesystem_source}/operations.cpp"
				"${_boost_filesystem_source}/directory.cpp"
				"${_boost_filesystem_source}/path.cpp"
				"${_boost_filesystem_source}/path_traits.cpp"
				"${_boost_filesystem_source}/portability.cpp"
				"${_boost_filesystem_source}/unique_path.cpp"
				"${_boost_filesystem_source}/utf8_codecvt_facet.cpp")
			add_library(Boost::filesystem ALIAS boost_filesystem)
			target_include_directories(boost_filesystem
				PUBLIC "${CMAKE_SOURCE_DIR}/external/boost"
				PRIVATE "${_boost_filesystem_source}")
			target_compile_definitions(boost_filesystem
				PUBLIC BOOST_FILESYSTEM_NO_LIB BOOST_FILESYSTEM_STATIC_LINK
				PRIVATE
					BOOST_FILESYSTEM_SOURCE
					BOOST_FILESYSTEM_DISABLE_SENDFILE
					BOOST_FILESYSTEM_DISABLE_COPY_FILE_RANGE
					BOOST_FILESYSTEM_DISABLE_STATX
					BOOST_FILESYSTEM_DISABLE_GETRANDOM
					BOOST_FILESYSTEM_DISABLE_ARC4RANDOM)
		endif()
		# Dynarmic performs its own header-only find_package(Boost) call. Seed the
		# standard discovery variables so it resolves this same pinned tree instead
		# of looking for a non-existent Switch system package.
		set(BOOST_ROOT "${CMAKE_SOURCE_DIR}/external/boost" CACHE PATH "" FORCE)
		set(BOOST_INCLUDEDIR "${CMAKE_SOURCE_DIR}/external/boost" CACHE PATH "" FORCE)
		set(Boost_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/external/boost" CACHE PATH "" FORCE)
		set(Boost_FOUND TRUE)
		set(Boost_VERSION "1.89.0")
		set(Boost_INCLUDE_DIRS "${CMAKE_SOURCE_DIR}/external/boost")
		set(Boost_LIBRARIES Boost::filesystem)
		message(STATUS "Switch: building Boost.Filesystem ${Boost_VERSION} from the pinned submodule")
	elseif(NOT VITA3K_FORCE_SYSTEM_BOOST)
		# find_package(Boost ...) setting
		set(Boost_USE_STATIC_LIBS ON)

		set(Boost_NO_WARN_NEW_VERSIONS 1)

		# First, try to find Boost without any hints (system Boost)
		if(NOT VITA3K_FORCE_CUSTOM_BOOST)
			find_package(Boost 1.81 COMPONENTS ${BOOST_MODULES_TO_FIND} QUIET)
		endif()

		# If system Boost hasn't been found, then enable hints to find custom Boost
		if (NOT Boost_FOUND)
			message(STATUS "A Boost distribution that meets the requirements needed for the project hasn't been found in your system. Falling back to custom Boost distribution...")

			# Set path hints for Boost search inside the repository folder and assist in compilation if needed
			set(BOOST_SOURCEDIR "${CMAKE_SOURCE_DIR}/external/boost")       # For internal use
			set(BOOST_INSTALLDIR "${CMAKE_BINARY_DIR}/external/boost")      # For internal use
			set(BOOST_ROOT "${BOOST_SOURCEDIR}")                            # find_package(Boost ...) hint
			set(BOOST_INCLUDEDIR "${BOOST_SOURCEDIR}")                      # find_package(Boost ...) hint
			set(BOOST_LIBRARYDIR "${BOOST_INSTALLDIR}/lib")                 # find_package(Boost ...) hint

			# Find Boost again to check for a existing compilation of the custom distribution
			find_package(Boost 1.81 COMPONENTS ${BOOST_MODULES_TO_FIND} PATHS ${BOOST_INSTALLDIR} NO_DEFAULT_PATH)

			# If no build of the custom distribution is found, compile it
			if(NOT Boost_FOUND)
				message(STATUS "No existing custom distribution of Boost has been found in ${BOOST_INSTALLDIR}. Proceeding to compile Boost...")

				# Set compilation toolset for b2 and Boost builds
				if (CMAKE_HOST_SYSTEM_NAME STREQUAL "Windows")
					if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
						if(CMAKE_GENERATOR_TOOLSET STREQUAL "ClangCL")
							# Proper toolset should be "clang-win", but b2 has a bug that makes it spam Visual Studio instances if set
							# Since Clang for Windows is ABI-compatible with MSVC, using MSVC is fine
							set(BOOST_TOOLSET "msvc")
						else()
							message(WARNING "Boost compilation on Windows requires the use of a clang-cl compiler running inside a Visual Studio Developer CLI environment with a Windows C++ SDK available.")
							set(BOOST_TOOLSET "msvc")
						endif()
					else()
						set(BOOST_TOOLSET "msvc")
					endif()
				elseif(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
					if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
						set(BOOST_TOOLSET "clang")
					elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
						set(BOOST_TOOLSET "gcc")
					endif()
				elseif(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
					set(BOOST_TOOLSET "clang-darwin")
					if (ARCHITECTURE STREQUAL "x86_64")
						set(BOOST_ARCHITECTURE "x86")
					elseif (ARCHITECTURE STREQUAL "arm64")
						set(BOOST_ARCHITECTURE "arm")
					else()
						set(BOOST_ARCHITECTURE "arm+x86")
					endif()
				endif()

				# Determine b2 executable name depending on host system
				if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Windows")
					set(b2_EXECUTABLE_NAME "b2.exe")
				else()
					set(b2_EXECUTABLE_NAME "b2")
				endif()

				# Find b2
				find_program(b2 NAME ${b2_EXECUTABLE_NAME} PATHS ${BOOST_SOURCEDIR} NO_CACHE NO_DEFAULT_PATH)

				# Compile b2 if it isn't found
				if(b2 STREQUAL "b2-NOTFOUND")
					message(STATUS "The b2 buildsystem executable hasn't been found in your system. Compiling b2...")
					b2_build()

					# Find b2 again after compilation
					find_program(b2 NAME ${b2_EXECUTABLE_NAME} PATHS ${BOOST_SOURCEDIR} NO_CACHE NO_DEFAULT_PATH REQUIRED)
				endif()

				# Compile Boost
				message(STATUS "Compiling Boost...")
				boost_compile()

				# Find Boost again
				find_package(Boost 1.81 COMPONENTS ${BOOST_MODULES_TO_FIND} PATHS ${BOOST_INSTALLDIR} NO_DEFAULT_PATH REQUIRED)
			else()
				message(STATUS "Custom Boost distribution found in ${BOOST_INSTALLDIR}")
			endif()
		endif()
	else()
		# Try to find Boost on the system and CMake's default paths
		set(Boost_USE_STATIC_LIBS ON)
		find_package(Boost 1.81 COMPONENTS ${BOOST_MODULES_TO_FIND} REQUIRED)
	endif()
endmacro(get_boost)

get_boost()

if(WIN32)
	add_compile_definitions("_SILENCE_ALL_CXX17_DEPRECATION_WARNINGS" "_CRT_SECURE_NO_WARNINGS" "NOMINMAX")
endif()

# Disable inclusion of timestamps in precompiled headers on Windows
if(CMAKE_HOST_WIN32 AND (CMAKE_CXX_COMPILER_ID STREQUAL "Clang"))
	string(APPEND CMAKE_C_FLAGS " -Xclang -fno-pch-timestamp")
	string(APPEND CMAKE_CXX_FLAGS " -Xclang -fno-pch-timestamp")
endif()

# Allow per-translation-unit parallel builds when using MSVC
if(CMAKE_GENERATOR MATCHES "Visual Studio" AND (CMAKE_C_COMPILER_ID MATCHES "MSVC|Intel|Clang" OR CMAKE_CXX_COMPILER_ID MATCHES "MSVC|Intel|Clang"))
	string(APPEND CMAKE_C_FLAGS " /MP")
	string(APPEND CMAKE_CXX_FLAGS " /MP")
endif()

if (${BUILD_APPIMAGE})
	if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
		set(LINUXDEPLOY_COMMAND "${CMAKE_SOURCE_DIR}/appimage/linuxdeploy.AppImage" CACHE INTERNAL "")
		if (NOT EXISTS "${LINUXDEPLOY_COMMAND}")
			message(FATAL_ERROR "Could not find linuxdeploy at ${LINUXDEPLOY_COMMAND}!")
		endif()
	else()
		message(FATAL_ERROR "Cannot build an AppImage for a non-Linux host.")
	endif()
endif()

add_subdirectory(external)
add_subdirectory(vita3k)
add_subdirectory(tools/gen-modules)
