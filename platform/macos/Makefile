# nodeTrix for macOS: the 3DS app's own code plus mac_platform.m, as a .app.
#
#   make -C platform/macos          -> build/nodeTrix.app (Apple silicon + Intel)
#   make -C platform/macos run      -> build it and open it
#   make -C platform/macos test     -> runs it with no window: screenshots + a sound check
#   make -C platform/macos zip      -> build/nodeTrix-macos.zip, ready to share
#
# Needs only Xcode's command line tools.

VERSION ?= 0.5.0
SRC     := ../../sequencer/source
COMMON  := ../../common
APP     := build/nodeTrix.app
BIN     := $(APP)/Contents/MacOS/nodeTrix
ICON_PNG := ../../docs/screenshots/icon-nodetrix.png

ARCHS   ?= -arch arm64 -arch x86_64
FLAGS   := -O2 $(ARCHS) -mmacosx-version-min=11.0
# NT_DESKTOP: real locks, and audio / mic from this folder. The app's main() becomes nodetrix_main().
APPFLAGS := $(FLAGS) -std=gnu11 -DNT_DESKTOP=1 -Dmain=nodetrix_main -Ishim -I$(SRC) -I$(COMMON) -include sdpath.h \
            -Wall -Wno-unused-function -Wno-format-truncation -Wno-unknown-warning-option -Wno-absolute-value
CSRC    := $(wildcard $(SRC)/*.c) $(COMMON)/netcfg.c $(COMMON)/osc.c
OBJS    := $(patsubst %.c,build/obj/%.o,$(notdir $(CSRC))) build/obj/mac_platform.o
FRAMEWORKS := -framework Cocoa -framework QuartzCore -framework CoreText -framework AudioToolbox -framework ImageIO

all: $(APP)

$(APP): $(BIN) $(APP)/Contents/Info.plist $(APP)/Contents/Resources/nodeTrix.icns
	xattr -cr $(APP)
	codesign --force -s - $(APP)
	@echo "built $(APP)"

$(BIN): $(OBJS)
	@mkdir -p $(dir $@)
	cc $(FLAGS) $(OBJS) $(FRAMEWORKS) -o $@

build/obj/%.o: $(SRC)/%.c $(wildcard $(SRC)/*.h) $(wildcard shim/*.h) sdpath.h
	@mkdir -p build/obj
	cc $(APPFLAGS) -c $< -o $@

build/obj/%.o: $(COMMON)/%.c $(wildcard $(COMMON)/*.h) $(wildcard shim/*.h) sdpath.h
	@mkdir -p build/obj
	cc $(APPFLAGS) -c $< -o $@

build/obj/mac_platform.o: mac_platform.m $(wildcard shim/*.h) $(SRC)/synth.h
	@mkdir -p build/obj
	cc $(FLAGS) -fobjc-arc -Wall -Ishim -I$(SRC) -c $< -o $@

$(APP)/Contents/Info.plist: Info.plist Makefile
	@mkdir -p $(dir $@)
	sed 's/@VERSION@/$(VERSION)/g' Info.plist > $@

$(APP)/Contents/Resources/nodeTrix.icns: $(ICON_PNG)
	@mkdir -p $(dir $@) build/nodeTrix.iconset
	for s in 16 32 128 256 512; do \
	  sips -z $$s $$s $< --out build/nodeTrix.iconset/icon_$${s}x$${s}.png >/dev/null; \
	  d=$$((s * 2)); sips -z $$d $$d $< --out build/nodeTrix.iconset/icon_$${s}x$${s}@2x.png >/dev/null; \
	done
	iconutil -c icns build/nodeTrix.iconset -o $@

run: $(APP)
	open $(APP)

# Runs the real app with no window and no sound: taps through the pages, saves
# each as a PNG in build/selftest and checks that the synth makes sound.
test: $(APP)
	rm -rf build/selftest && mkdir -p build/selftest
	NT_SELFTEST=$(CURDIR)/build/selftest $(BIN)

zip: $(APP)
	cd build && rm -f nodeTrix-macos.zip && ditto -c -k --keepParent nodeTrix.app nodeTrix-macos.zip
	@echo "built build/nodeTrix-macos.zip"

clean:
	rm -rf build

.PHONY: all run test zip clean
