# Conduit build — TrimUI Brick, Brick Hammer and Brick Pro (TrimUI OS, MustardOS and KNULLI)
#   make                 cross-compile for the TrimUI with zig (aarch64, glibc 2.28, against the device's SDL2)
#                        -> build/trimui/conduit
#   make desktop         a build for this computer (needs the SDL2 dev files) -> build/desktop/conduit
#                        (run it windowed: CONDUIT_WINDOWED=1 build/desktop/conduit)
#   make test            DSP + WAV unit tests (ASan/UBSan)
#   make test-e2e        the desktop build, run headless on SDL's disk audio driver
#   make package         the TrimUI build + build/Conduit-trimui.zip, to unzip at the root of the SD card
#                        (it holds Apps/Conduit), build/Conduit-mustardos.muxapp for MustardOS and
#                        build/Conduit-knulli.zip for KNULLI (the same binary)
#   make install SD=/path/to/card [OS=mustardos|knulli]   copy the app onto a mounted card
#   make version         print the version: CONDUIT_VERSION in src/main.c, or VERSION=x.y.z when given
#                        (CI passes the release tag's version, so a release is built as v<tag>)
#
# zig: ZIG=/path/to/zig, or `pip install ziglang==0.14.1` (used as `python3 -m ziglang`)

ZIG      ?= $(shell command -v zig 2>/dev/null || echo python3 -m ziglang)
ZTARGET  := aarch64-linux-gnu.2.28
CPU      := cortex_a53
APP      := conduit
APPDIR   := Apps/Conduit
SDL_INC  := third_party/SDL2-2.30.9-headers
SRC      := src/main.c src/dsp.c src/wav.c src/midi.c src/link.c src/bloom.c src/midiverb2.c src/verb.c
HDR      := $(wildcard src/*.h)
VERSION  ?= $(shell sed -n 's/^\#define CONDUIT_VERSION "\([^"]*\)".*/\1/p' src/main.c)

WARN     := -Wall -Wextra -Wno-unused-parameter
CFLAGS   ?= -O2
CFLAGS   += -std=gnu11 $(WARN) -ffast-math -DCONDUIT_VERSION='"$(VERSION)"'

# changes only when the version does, so a new VERSION= rebuilds what has it compiled in
build/version: FORCE
	@mkdir -p build
	@test "$$(cat $@ 2>/dev/null)" = "$(VERSION)" || echo $(VERSION) > $@
FORCE:

# --- the TrimUI build
DEV_CC   = $(ZIG) cc -target $(ZTARGET) -mcpu=$(CPU)
DEV_OBJ  := $(patsubst src/%.c,build/trimui/%.o,$(SRC))

all: build/trimui/$(APP)

build/trimui/$(APP): $(DEV_OBJ) build/sdl2/libSDL2-2.0.so.0
	$(DEV_CC) -o $@ $(DEV_OBJ) build/sdl2/libSDL2-2.0.so.0 -lm -lpthread -Wl,--allow-shlib-undefined -s

build/trimui/%.o: src/%.c $(HDR) build/version | build/trimui
	$(DEV_CC) $(CFLAGS) -I$(SDL_INC) -c -o $@ $<

# a stand-in for the device's libSDL2-2.0.so.0, to link against (the firmware has the real one)
build/sdl2/libSDL2-2.0.so.0: tools/mksdlstub.py $(SRC)
	@mkdir -p build/sdl2
	python3 tools/mksdlstub.py build/sdl2/sdl2_stub.c $(SRC)
	$(ZIG) cc -target $(ZTARGET) -shared -Wl,-soname,libSDL2-2.0.so.0 -o $@ build/sdl2/sdl2_stub.c

# --- a desktop build, for trying it and for the end-to-end test
HOST_CC  ?= cc
HOST_OBJ := $(patsubst src/%.c,build/desktop/%.o,$(SRC))

desktop: build/desktop/$(APP)

build/desktop/$(APP): $(HOST_OBJ)
	$(HOST_CC) -o $@ $(HOST_OBJ) $(shell sdl2-config --libs) -lpthread -lm

build/desktop/%.o: src/%.c $(HDR) build/version | build/desktop
	$(HOST_CC) $(CFLAGS) $(shell sdl2-config --cflags | sed 's|-I\([^ ]*\)/SDL2|-I\1|') -c -o $@ $<

build/trimui build/desktop:
	mkdir -p $@

# --- tests (on this computer)
build/test_dsp: tests/test_dsp.c src/dsp.c src/wav.c src/midi.c src/link.c src/link.h src/bloom.c src/midiverb2.c src/verb.c src/dsp.h src/wav.h src/midi.h src/bloom.h src/midiverb2.h src/verb.h
	@mkdir -p build
	$(HOST_CC) -std=gnu11 -O1 -g -Wall -Wextra -fsanitize=address,undefined -o $@ tests/test_dsp.c src/dsp.c src/wav.c src/midi.c src/link.c src/bloom.c src/midiverb2.c src/verb.c -lm -lpthread

test: build/test_dsp
	build/test_dsp

test-e2e: build/desktop/$(APP)
	./tests/e2e.sh build/desktop/$(APP)

# the embedded glyphs (committed; regenerate with `make font`)
font:
	python3 tools/genfont.py /usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf src/font_data.h

# --- the SD card packages
package: build/Conduit-trimui.zip build/Conduit-mustardos.muxapp build/Conduit-knulli.zip

# TrimUI OS: Apps/Conduit, as TrimUI OS lists apps
build/Conduit-trimui.zip: build/trimui/$(APP) $(shell find $(APPDIR) -type f)
	rm -rf build/sd $@
	mkdir -p build/sd/Apps
	cp -r $(APPDIR) build/sd/Apps/
	rm -f build/sd/$(APPDIR)/conduit.cfg build/sd/$(APPDIR)/log.txt build/sd/$(APPDIR)/conduit_diag.txt \
		build/sd/$(APPDIR)/recordings/*.wav build/sd/$(APPDIR)/$(APP)
	cp build/trimui/$(APP) build/sd/$(APPDIR)/$(APP)
	cp LICENSE build/sd/$(APPDIR)/LICENSE.txt
	cp README.md build/sd/$(APPDIR)/README.md
	echo $(VERSION) > build/sd/$(APPDIR)/VERSION
	chmod +x build/sd/$(APPDIR)/launch.sh build/sd/$(APPDIR)/conduit_diag.sh build/sd/$(APPDIR)/$(APP)
	cd build/sd && zip -qr ../Conduit-trimui.zip Apps
	@echo "$@ ready: unzip it at the root of the TrimUI's SD card"

# MustardOS: a .muxapp is a zip that its Archive Manager unpacks into the application folder (MUOS/application on
# the card), and the folder's name is the name in the Applications list. The same app folder and binary, plus
# mux_launch.sh (how MustardOS starts an app), wake_screen.sh (the Brick Pro's screen can stay dark otherwise)
# and the list glyph. Keep mux_launch.sh out of the TrimUI OS zip.
MUX := mustardos
build/Conduit-mustardos.muxapp: build/Conduit-trimui.zip $(shell find $(MUX) -type f)
	rm -rf build/mux $@
	mkdir -p build/mux/Conduit
	cp -r build/sd/$(APPDIR)/. build/mux/Conduit/
	cp -r $(MUX)/. build/mux/Conduit/
	chmod +x build/mux/Conduit/mux_launch.sh build/mux/Conduit/wake_screen.sh
	cd build/mux && zip -qr ../Conduit-mustardos.muxapp Conduit
	@echo "$@ ready: copy it into ARCHIVE on the MustardOS card and install it from the Archive Manager"

# KNULLI: Conduit.sh and conduit/ go in /userdata/roms/ports (the same app folder and binary)
build/Conduit-knulli.zip: build/Conduit-trimui.zip knulli/Conduit.sh
	rm -rf build/knulli $@
	mkdir -p build/knulli/conduit
	cp -r build/sd/$(APPDIR)/. build/knulli/conduit/
	rm -f build/knulli/conduit/launch.sh build/knulli/conduit/config.json build/knulli/conduit/icon.png
	cp knulli/Conduit.sh build/knulli/
	chmod +x build/knulli/Conduit.sh
	cd build/knulli && zip -qr ../Conduit-knulli.zip Conduit.sh conduit
	@echo "$@ ready: unzip it into /userdata/roms/ports on the KNULLI card"

OS ?= trimui
ifeq ($(OS),mustardos)
INSTALL_DIR = $(SD)/MUOS/application/Conduit
INSTALL_ROOT = $(SD)/MUOS
INSTALL_FROM = build/mux/Conduit
else ifeq ($(OS),knulli)
INSTALL_DIR = $(SD)/roms/ports
INSTALL_ROOT = $(SD)/roms/ports
INSTALL_FROM = build/knulli
else
INSTALL_DIR = $(SD)/$(APPDIR)
INSTALL_ROOT = $(SD)/Apps
INSTALL_FROM = build/sd/$(APPDIR)
endif
install: package
	@test -n "$(SD)" || (echo "usage: make install SD=/path/to/mounted/sdcard [OS=mustardos|knulli]" && exit 1)
	@test -d "$(INSTALL_ROOT)" || (echo "$(INSTALL_ROOT) not found - is this the $(OS) card?" && exit 1)
	mkdir -p "$(INSTALL_DIR)"
	cp -r $(INSTALL_FROM)/. "$(INSTALL_DIR)/"
	sync

version:
	@echo $(VERSION)

clean:
	rm -rf build

.PHONY: FORCE all desktop test test-e2e font package install version clean
