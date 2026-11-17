#---------------------------------------------------------------------------------
.SUFFIXES:
#---------------------------------------------------------------------------------

# Default to the standard devkitPro install location if not set in the shell
export DEVKITPRO ?= /opt/devkitpro
export DEVKITARM ?= $(DEVKITPRO)/devkitARM

ifeq ($(wildcard $(DEVKITARM)/3ds_rules),)
$(error "devkitARM not found at $(DEVKITARM). Install devkitPro or set DEVKITARM")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITARM)/3ds_rules

#---------------------------------------------------------------------------------
# App metadata (shown in the Homebrew Launcher)
#---------------------------------------------------------------------------------
APP_TITLE       := nodeTrix
APP_DESCRIPTION := 3D sequencer + modular synth
APP_AUTHOR      := thomas

TARGET      := nodeTrix
BUILD       := build
SOURCES     := source ../common
DATA        := data
INCLUDES    := include ../common
GRAPHICS    := gfx
ROMFS       := romfs

# UI font: Arial from macOS, converted to the 3DS bcfnt format (ASCII + a few symbols)
FONT_SRC    ?= /System/Library/Fonts/Supplemental/Arial.ttf
FONT_OUT    := romfs/arial.bcfnt

#---------------------------------------------------------------------------------
# options for code generation
#---------------------------------------------------------------------------------
ARCH    := -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft

CFLAGS  := -g -Wall -O2 -mword-relocations -ffunction-sections $(ARCH)
CFLAGS  += $(INCLUDE) -D__3DS__
CXXFLAGS := $(CFLAGS) -fno-rtti -fno-exceptions -std=gnu++11
ASFLAGS := -g $(ARCH)
LDFLAGS  = -specs=3dsx.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)

LIBS    := -lcitro2d -lcitro3d -lctru -lm
LIBDIRS := $(CTRULIB)

#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))
#---------------------------------------------------------------------------------

export OUTPUT   := $(CURDIR)/$(TARGET)
export TOPDIR   := $(CURDIR)
export VPATH    := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
                   $(foreach dir,$(DATA),$(CURDIR)/$(dir))
export DEPSDIR  := $(CURDIR)/$(BUILD)

CFILES   := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES   := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))

export LD := $(CC)
export OFILES_SOURCES := $(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)
export OFILES   := $(OFILES_SOURCES)
export HFILES   :=
export INCLUDE  := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
                   $(foreach dir,$(LIBDIRS),-I$(dir)/include) \
                   -I$(CURDIR)/$(BUILD)
export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

ifeq ($(strip $(ICON)),)
	icons := $(wildcard *.png)
	ifneq (,$(findstring icon.png,$(icons)))
		export APP_ICON := $(TOPDIR)/icon.png
	else
		export APP_ICON := $(CTRULIB)/default_icon.png
	endif
else
	export APP_ICON := $(TOPDIR)/$(ICON)
endif

export _3DSXFLAGS += --smdh=$(CURDIR)/$(TARGET).smdh

ifneq ($(ROMFS),)
	export _3DSXFLAGS += --romfs=$(CURDIR)/$(ROMFS)
endif

.PHONY: all clean run send

all: $(FONT_OUT) $(BUILD)
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

# Preview in the Azahar emulator. Override with: make run AZAHAR=/path/to/Azahar.app
AZAHAR ?= $(firstword $(wildcard /Applications/Azahar.app $(HOME)/Applications/Azahar.app $(HOME)/Downloads/azahar-*/Azahar.app))

run: all
	@test -n "$(AZAHAR)" || (echo "Azahar.app not found; pass AZAHAR=/path/to/Azahar.app" && exit 1)
	open -n -a "$(AZAHAR)" --args "$(CURDIR)/$(TARGET).3dsx"

# Send to a real 3DS: open Homebrew Launcher, press Y, then run this.
# If auto-discovery fails, pass the IP shown on the 3DS: make send IP=192.168.0.50
send: all
	$(DEVKITPRO)/tools/bin/3dslink $(if $(IP),-a $(IP)) $(TARGET).3dsx

$(BUILD):
	@mkdir -p $@

$(FONT_OUT): gfx/font_chars.txt
	@mkdir -p romfs
	$(DEVKITPRO)/tools/bin/mkbcfnt -s 24 -w $< -o $@ "$(FONT_SRC)"

clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).3dsx $(TARGET).smdh $(TARGET).elf

#---------------------------------------------------------------------------------
else
#---------------------------------------------------------------------------------

$(OUTPUT).3dsx : $(OUTPUT).elf $(OUTPUT).smdh
$(OUTPUT).elf  : $(OFILES)

-include $(DEPSDIR)/*.d

#---------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------
