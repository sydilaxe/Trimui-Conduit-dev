# Conduit build — TrimUI Brick, Brick Hammer and Brick Pro, running MustardOS
#   make                 cross-compile for the TrimUI with zig (aarch64, glibc 2.28, against the device's SDL2)
#                        -> build/trimui/conduit
#   make desktop         a build for this computer (needs the SDL2 dev files) -> build/desktop/conduit
#                        (run it windowed: CONDUIT_WINDOWED=1 build/desktop/conduit)
#   make test            DSP + WAV unit tests (ASan/UBSan)
#   make test-e2e        the desktop build, run headless on SDL's disk audio driver
#   make package         the TrimUI build + build/Conduit-mustardos.muxapp, the MustardOS application archive
#   make install SD=/path/to/card   copy the app onto a mounted MustardOS card (MUOS/application/Conduit)
#   make update-test     install a fake release over a test app folder, the way the app updates itself
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
SRC      := src/main.c src/dsp.c src/wav.c src/midi.c src/link.c src/bloom.c src/midiverb2.c src/verb.c \
            src/net.c src/update.c
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

# --- Mbed TLS (Apache-2.0) for the updater's HTTPS, from a pinned release, not committed; each toolchain
# builds its own libmbedtls.a from it with tools/mbedtls.mk (as in Columbia Street, RadioScan and Jaffedrone)
MBED_VER := 3.6.7
MBED_SHA := a7e8bcbec0e6f761b4af24f25677626b35f762f68eef79c08677a363212d11f6
MBED_DIR := third_party/mbedtls
MBED_URL := https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-$(MBED_VER)/mbedtls-$(MBED_VER).tar.bz2
MBED_SRC := $(MBED_DIR)/.v$(MBED_VER)
$(MBED_SRC):
	rm -rf $(MBED_DIR) && mkdir -p $(MBED_DIR)
	curl -sSL -o $(MBED_DIR).tar.bz2 $(MBED_URL) || wget -qO $(MBED_DIR).tar.bz2 $(MBED_URL)
	echo "$(MBED_SHA)  $(MBED_DIR).tar.bz2" | sha256sum -c --quiet || (rm -f $(MBED_DIR).tar.bz2; false)
	tar -xjf $(MBED_DIR).tar.bz2 -C $(MBED_DIR) --strip-components=1
	rm $(MBED_DIR).tar.bz2
	touch $@

build/zig/libmbedtls.a: $(MBED_SRC)
	$(MAKE) -f tools/mbedtls.mk OUT=build/zig MBED_DIR=$(MBED_DIR) \
		CC="$(ZIG) cc -target $(ZTARGET) -mcpu=$(CPU)" AR="$(ZIG) ar" MBED_CFLAGS=-O2

build/host/libmbedtls.a: $(MBED_SRC)
	$(MAKE) -f tools/mbedtls.mk OUT=build/host MBED_DIR=$(MBED_DIR) CC="$(HOST_CC)" AR=ar MBED_CFLAGS=-O2

HOST_CC  ?= cc

# --- the TrimUI build (CONDUIT_TRIMUI: it checks GitHub for updates by default)
DEV_CC   = $(ZIG) cc -target $(ZTARGET) -mcpu=$(CPU)
DEV_OBJ  := $(patsubst src/%.c,build/trimui/%.o,$(SRC))

all: build/trimui/$(APP)

build/trimui/$(APP): $(DEV_OBJ) build/sdl2/libSDL2-2.0.so.0 build/zig/libmbedtls.a
	$(DEV_CC) -o $@ $(DEV_OBJ) build/zig/libmbedtls.a build/sdl2/libSDL2-2.0.so.0 -lm -lpthread -Wl,--allow-shlib-undefined -s

build/trimui/%.o: src/%.c $(HDR) build/version $(MBED_SRC) | build/trimui
	$(DEV_CC) $(CFLAGS) -DCONDUIT_TRIMUI -I$(SDL_INC) -I$(MBED_DIR)/include -c -o $@ $<

# a stand-in for the device's libSDL2-2.0.so.0, to link against (the firmware has the real one)
build/sdl2/libSDL2-2.0.so.0: tools/mksdlstub.py $(SRC)
	@mkdir -p build/sdl2
	python3 tools/mksdlstub.py build/sdl2/sdl2_stub.c $(SRC)
	$(ZIG) cc -target $(ZTARGET) -shared -Wl,-soname,libSDL2-2.0.so.0 -o $@ build/sdl2/sdl2_stub.c

# --- a desktop build, for trying it and for the end-to-end test
HOST_OBJ := $(patsubst src/%.c,build/desktop/%.o,$(SRC))

desktop: build/desktop/$(APP)

build/desktop/$(APP): $(HOST_OBJ) build/host/libmbedtls.a
	$(HOST_CC) -o $@ $(HOST_OBJ) build/host/libmbedtls.a $(shell sdl2-config --libs) -lpthread -lm

build/desktop/%.o: src/%.c $(HDR) build/version $(MBED_SRC) | build/desktop
	$(HOST_CC) $(CFLAGS) $(shell sdl2-config --cflags | sed 's|-I\([^ ]*\)/SDL2|-I\1|') -I$(MBED_DIR)/include -c -o $@ $<

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

# the host updater: what the app does on A, from the command line (src/main_update.c)
build/conduit_update: src/net.c src/update.c src/main_update.c src/net.h src/update.h build/version build/host/libmbedtls.a
	$(HOST_CC) -std=gnu11 -O2 -Wall -Wextra -DCONDUIT_VERSION='"$(VERSION)"' -I$(MBED_DIR)/include -o $@ \
		src/net.c src/update.c src/main_update.c build/host/libmbedtls.a -lpthread

update-test: build/conduit_update
	sh tools/update_test.sh

# the embedded glyphs (committed; regenerate with `make font`)
font:
	python3 tools/genfont.py /usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf src/font_data.h

# --- the package. MustardOS: a .muxapp is a zip that its Archive Manager unpacks into the application folder
# (MUOS/application on the card), and the folder's name is the name in the Applications list. The app folder
# (Apps/Conduit in this repo) and binary, plus mux_launch.sh (how MustardOS starts an app), wake_screen.sh (the
# Brick Pro's screen can stay dark otherwise) and the list glyph.
package: build/Conduit-mustardos.muxapp

MUX := mustardos
build/Conduit-mustardos.muxapp: build/trimui/$(APP) $(shell find $(APPDIR) $(MUX) -type f) LICENSE README.md $(MBED_SRC)
	rm -rf build/mux $@
	mkdir -p build/mux/Conduit
	cp -r $(APPDIR)/. build/mux/Conduit/
	cp -r $(MUX)/. build/mux/Conduit/
	rm -f build/mux/Conduit/conduit.cfg build/mux/Conduit/log.txt build/mux/Conduit/conduit_diag.txt \
		build/mux/Conduit/recordings/*.wav
	cp build/trimui/$(APP) build/mux/Conduit/$(APP)
	cp LICENSE build/mux/Conduit/LICENSE.txt
	cp $(MBED_DIR)/LICENSE build/mux/Conduit/LICENSE-mbedtls.txt # the binary carries Mbed TLS
	cp README.md build/mux/Conduit/README.md
	echo $(VERSION) > build/mux/Conduit/VERSION
	chmod +x build/mux/Conduit/launch.sh build/mux/Conduit/conduit_diag.sh build/mux/Conduit/$(APP) \
		build/mux/Conduit/mux_launch.sh build/mux/Conduit/wake_screen.sh
	cd build/mux && zip -qr ../Conduit-mustardos.muxapp Conduit
	@echo "$@ ready: copy it into ARCHIVE on the MustardOS card and install it from the Archive Manager"

INSTALL_DIR = $(SD)/MUOS/application/Conduit
INSTALL_ROOT = $(SD)/MUOS
INSTALL_FROM = build/mux/Conduit
install: package
	@test -n "$(SD)" || (echo "usage: make install SD=/path/to/mounted/sdcard" && exit 1)
	@test -d "$(INSTALL_ROOT)" || (echo "$(INSTALL_ROOT) not found - is this the MustardOS card?" && exit 1)
	mkdir -p "$(INSTALL_DIR)"
	cp -r $(INSTALL_FROM)/. "$(INSTALL_DIR)/"
	sync

version:
	@echo $(VERSION)

clean:
	rm -rf build

.PHONY: FORCE all desktop test test-e2e update-test font package install version clean
