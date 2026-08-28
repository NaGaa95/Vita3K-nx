#---------------------------------------------------------------------------------
# Vita3K launcher -- standalone SDL2 GUI .nro that scans the installed Vita games,
# writes sdmc:/switch/vita3k/config.yml and chainloads the emulator (Vita3K.nro)
# with "-r <TITLEID>" via envSetNextLoad. Kept OUT of the emulator source tree so
# its SDL2 link never touches libemucore. Based on the devkitPro switch sdl2-demo
# template.
#---------------------------------------------------------------------------------
.SUFFIXES:
.DEFAULT_GOAL := all

ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment. export DEVKITPRO=<path to>/devkitpro")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITPRO)/libnx/switch_rules
include $(TOPDIR)/version.mk

TARGET		:=	Vita3K
BUILD		:=	build
SOURCES		:=	source
DATA		:=	data
INCLUDES	:=	include
ROMFS		:=	romfs		# launcher assets plus the bundled emulator (romfs/emu/Vita3K.nro)

# Pinned USB mass-storage + SMB dependencies are built out-of-tree, but kept
# inside the launcher directory so a normal checkout is self-contained.  The
# paths remain overridable for CI or for developers sharing a compatible cache.
STORAGE_DEPS_SOURCE ?= $(TOPDIR)/dependencies
STORAGE_BUILD ?= $(STORAGE_DEPS_SOURCE)/build
STORAGE_CMAKE ?= cmake
STORAGE_CMAKE_GENERATOR ?= Ninja
STORAGE_TOOLCHAIN ?= $(DEVKITPRO)/cmake/Switch.cmake
STORAGE_CMAKE_ARGS ?=

STORAGE_CONFIG_STAMP := $(STORAGE_BUILD)/.vita3k-storage-configured
STORAGE_BUILD_STAMP := $(STORAGE_BUILD)/.vita3k-storage-built
STORAGE_PATCH_INPUTS := $(wildcard $(STORAGE_DEPS_SOURCE)/libusbhsfs/*.patch)
STORAGE_UASP_INPUTS := $(STORAGE_DEPS_SOURCE)/libusbhsfs/usbhsfs_uasp.c \
	$(STORAGE_DEPS_SOURCE)/libusbhsfs/usbhsfs_uasp.h
STORAGE_INCLUDE := $(STORAGE_BUILD)/include
LIBSMB2_ARCHIVE := $(STORAGE_BUILD)/_deps/libsmb2-build/lib/libsmb2.a
LIBUSBHSFS_ARCHIVE := $(STORAGE_BUILD)/_deps/libusbhsfs-build/liblibusbhsfs.a
STORAGE_LIBS := $(LIBSMB2_ARCHIVE) $(LIBUSBHSFS_ARCHIVE)

APP_TITLE	:=	Vita3K
APP_AUTHOR	:=	naga
APP_VERSION	:=	$(VITA3K_NX_VERSION)

EMU_NRO_BUNDLED := $(CURDIR)/romfs/emu/Vita3K.nro
EMU_HASH_BUNDLED := $(CURDIR)/romfs/emu/Vita3K.sha256

#---------------------------------------------------------------------------------
ARCH	:=	-march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE

PKGS	:=	sdl2 SDL2_ttf SDL2_image libcurl

CFLAGS	:=	`$(PREFIX)pkg-config --cflags $(PKGS)` -Wall -Wextra -O3 -flto=auto -DNDEBUG -ffunction-sections \
			-fno-ident -ffile-prefix-map=$(CURDIR)=. -fmacro-prefix-map=$(CURDIR)=. \
			$(ARCH) -DVITA3K_NX_VERSION='"$(APP_VERSION)"' $(DEFINES)
CFLAGS	+=	$(INCLUDE) -D__SWITCH__ -D_GNU_SOURCE -D_LARGEFILE64_SOURCE -D_FILE_OFFSET_BITS=64

CXXFLAGS	:= $(CFLAGS) -std=gnu++17

ASFLAGS	:=	$(ARCH)
LDFLAGS	=	-specs=$(DEVKITPRO)/libnx/switch.specs $(ARCH) -Wl,-Map,$(notdir $*.map) \
			-flto=auto -Wl,--gc-sections -Wl,--build-id=none

# SDL2_image (covers) + libcurl (SteamGridDB) + turbojpeg (forwarder NACP icons).
# Keep the two storage archives ahead of their system-library dependencies and
# in the same order as the CMake interface target (libsmb2, then libusbhsfs).
LIBS	:=	$(STORAGE_LIBS) `$(PREFIX)pkg-config --libs $(PKGS)` -lturbojpeg -lz -lGLESv2 -lnx

#---------------------------------------------------------------------------------
LIBDIRS	:= $(PORTLIBS) $(LIBNX)

#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))
#---------------------------------------------------------------------------------

export OUTPUT	:=	$(CURDIR)/$(TARGET)
export TOPDIR	:=	$(CURDIR)

export VPATH	:=	$(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
			$(foreach dir,$(DATA),$(CURDIR)/$(dir))

export DEPSDIR	:=	$(CURDIR)/$(BUILD)

CFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES	:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
BINFILES	:=	$(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.*)))

#---------------------------------------------------------------------------------
# use CXX for linking C++ and libEGL dependent projects
#---------------------------------------------------------------------------------
export LD	:=	$(CXX)

export OFILES_BIN	:=	$(addsuffix .o,$(BINFILES))
export OFILES_SRC	:=	$(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)
export OFILES 	:=	$(OFILES_BIN) $(OFILES_SRC)
export HFILES_BIN	:=	$(addsuffix .h,$(subst .,_,$(BINFILES)))

	export INCLUDE	:=	$(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
			$(foreach dir,$(LIBDIRS),-I$(dir)/include) \
			-I$(STORAGE_INCLUDE) \
			-I$(CURDIR)/$(BUILD)

export LIBPATHS	:=	$(foreach dir,$(LIBDIRS),-L$(dir)/lib)

ifeq ($(strip $(ICON)),)
	icons := $(wildcard *.jpg)
	ifneq (,$(findstring $(TARGET).jpg,$(icons)))
		export APP_ICON := $(TOPDIR)/$(TARGET).jpg
	else
		ifneq (,$(findstring icon.jpg,$(icons)))
			export APP_ICON := $(TOPDIR)/icon.jpg
		endif
	endif
else
	export APP_ICON := $(TOPDIR)/$(ICON)
endif

ifeq ($(strip $(NO_ICON)),)
	export NROFLAGS += --icon=$(APP_ICON)
endif

ifeq ($(strip $(NO_NACP)),)
	export NROFLAGS += --nacp=$(CURDIR)/$(TARGET).nacp
endif

ifneq ($(strip $(ROMFS)),)
	export NROFLAGS += --romfsdir=$(CURDIR)/$(ROMFS)
endif

ifneq ($(APP_TITLEID),)
	export NACPFLAGS += --titleid=$(APP_TITLEID)
endif

.PHONY: $(BUILD) clean all launcher-elf prepare-romfs forwarder-assets storage-deps storage-deps-force

# CMake only runs when its pinned input/toolchain changed or its cache vanished;
# Ninja only runs when configuration changed or an expected output went missing.
storage-deps-force:

$(STORAGE_CONFIG_STAMP): $(STORAGE_DEPS_SOURCE)/CMakeLists.txt $(STORAGE_TOOLCHAIN) $(STORAGE_PATCH_INPUTS) storage-deps-force
	@set -e; \
	inputs_newer=false; for input in $(STORAGE_PATCH_INPUTS); do \
		if [ "$$input" -nt "$@" ]; then inputs_newer=true; fi; \
	done; \
	if [ ! -f "$@" ] || $$inputs_newer || \
	   [ ! -f "$(STORAGE_BUILD)/CMakeCache.txt" ] || \
	   [ ! -f "$(STORAGE_BUILD)/build.ninja" ] || \
	   [ "$(STORAGE_DEPS_SOURCE)/CMakeLists.txt" -nt "$@" ] || \
	   [ "$(STORAGE_TOOLCHAIN)" -nt "$@" ]; then \
		echo "configuring pinned launcher storage dependencies"; \
		[ -d "$(STORAGE_BUILD)" ] || mkdir -p "$(STORAGE_BUILD)"; \
		$(STORAGE_CMAKE) -S "$(STORAGE_DEPS_SOURCE)" -B "$(STORAGE_BUILD)" \
			-G "$(STORAGE_CMAKE_GENERATOR)" \
			-DCMAKE_TOOLCHAIN_FILE="$(STORAGE_TOOLCHAIN)" \
			-DCMAKE_BUILD_TYPE=Release $(STORAGE_CMAKE_ARGS); \
		touch "$@"; \
	fi

$(STORAGE_BUILD_STAMP): $(STORAGE_CONFIG_STAMP) $(STORAGE_UASP_INPUTS) storage-deps-force
	@set -e; \
	if [ ! -f "$@" ] || [ "$(STORAGE_CONFIG_STAMP)" -nt "$@" ] || \
	   [ "$(word 1,$(STORAGE_UASP_INPUTS))" -nt "$@" ] || \
	   [ "$(word 2,$(STORAGE_UASP_INPUTS))" -nt "$@" ] || \
	   [ ! -f "$(LIBSMB2_ARCHIVE)" ] || [ ! -f "$(LIBUSBHSFS_ARCHIVE)" ] || \
	   [ ! -f "$(STORAGE_INCLUDE)/smb2/libsmb2.h" ] || \
	   [ ! -f "$(STORAGE_INCLUDE)/usbhsfs.h" ]; then \
		echo "building pinned launcher storage dependencies"; \
		$(STORAGE_CMAKE) --build "$(STORAGE_BUILD)" --target smb2 libusbhsfs; \
		test -f "$(LIBSMB2_ARCHIVE)"; \
		test -f "$(LIBUSBHSFS_ARCHIVE)"; \
		test -f "$(STORAGE_INCLUDE)/smb2/libsmb2.h"; \
		test -f "$(STORAGE_INCLUDE)/usbhsfs.h"; \
		touch "$@"; \
	fi

storage-deps: $(STORAGE_BUILD_STAMP)

# Compile/link validation does not require a bundled emulator or final RomFS.
launcher-elf: storage-deps
	@[ -d $(BUILD) ] || mkdir -p $(BUILD)
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile $(OUTPUT).elf

#---------------------------------------------------------------------------------
all: forwarder-assets prepare-romfs storage-deps $(BUILD)

$(BUILD): forwarder-assets prepare-romfs storage-deps

# The HOME shortcut template carries the same author/version as the launcher.
# Build it before assembling RomFS so a version or metadata change cannot leave
# a stale default.nacp/hbl.nso embedded in an otherwise fresh launcher NRO.
forwarder-assets:
	@$(MAKE) --no-print-directory -C $(TOPDIR)/fwd

prepare-romfs: $(EMU_HASH_BUNDLED)

$(EMU_HASH_BUNDLED): $(EMU_NRO_BUNDLED)
	@sha256sum "$<" | cut -d ' ' -f 1 > "$@.tmp"
	@new_hash=; old_hash=; \
	IFS= read -r new_hash < "$@.tmp"; \
	if [ -f "$@" ]; then IFS= read -r old_hash < "$@"; fi; \
	if [ "$$new_hash" = "$$old_hash" ]; then \
		rm -f "$@.tmp" && touch -r "$<" "$@"; \
	else \
		mv -f "$@.tmp" "$@"; \
	fi

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

#---------------------------------------------------------------------------------
clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).nro $(TARGET).nacp $(TARGET).elf

#---------------------------------------------------------------------------------
else
.PHONY:	all

DEPENDS	:=	$(OFILES:.o=.d)

#---------------------------------------------------------------------------------
all	:	$(OUTPUT).nro

ifeq ($(strip $(NO_NACP)),)
$(OUTPUT).nro	:	$(OUTPUT).elf $(OUTPUT).nacp
else
$(OUTPUT).nro	:	$(OUTPUT).elf
endif

# elf2nro embeds RomFS directly, so make cannot infer that a freshly rebuilt
# emulator payload must regenerate the outer launcher NRO.
ifneq ($(strip $(ROMFS)),)
$(OUTPUT).nro	:	$(TOPDIR)/$(strip $(ROMFS))/emu/Vita3K.nro \
		$(TOPDIR)/$(strip $(ROMFS))/emu/Vita3K.sha256
endif

$(OUTPUT).elf	:	$(OFILES) $(STORAGE_LIBS)

# Version changes affect compiled user agents/updater metadata and the generated NACP.
$(OFILES_SRC) $(OUTPUT).nacp : $(TOPDIR)/version.mk

$(OFILES_SRC)	: $(HFILES_BIN)

%.bin.o	%_bin.h :	%.bin
	@echo $(notdir $<)
	@$(bin2o)

-include $(DEPENDS)

#---------------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------------
