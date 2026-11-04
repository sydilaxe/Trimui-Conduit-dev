# Conduit build
#   make                      native build (uses system SDL2 headers if found)
#   make CROSS=aarch64-linux-gnu-   cross build for TrimUI Brick / Brick Pro
#   make test                 DSP + WAV unit tests
#   make test-e2e             headless end-to-end run on SDL's disk audio driver
#   make package              build dist/ zips for MustardOS and Knulli

CROSS ?=
CC := $(CROSS)gcc
TARGET ?= conduit
CFLAGS ?= -O2 -g0
CFLAGS += -std=gnu11 -Wall -Wextra -Wno-unused-parameter -ffast-math
LDLIBS := -lm

SDL_CFLAGS := $(shell pkg-config --cflags sdl2 2>/dev/null)
SDL_LIBS := $(shell pkg-config --libs sdl2 2>/dev/null)
ifneq ($(SDL_LIBS),)
  CFLAGS += -DUSE_SYSTEM_SDL $(SDL_CFLAGS)
  LDLIBS += $(SDL_LIBS)
else
  # no headers: use src/sdl_shim.h and link the runtime library directly
  LDLIBS += -l:libSDL2-2.0.so.0
endif

ifneq ($(CROSS),)
  CFLAGS += -mcpu=cortex-a53
endif

SRC := src/main.c src/dsp.c src/wav.c
VERSION := $(shell sed -n 's/^\#define VERSION "\(.*\)"/\1/p' src/main.c)

all: $(TARGET)

$(TARGET): $(SRC) src/*.h
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDFLAGS) $(LDLIBS)

src/font_data.h:
	python3 tools/genfont.py /usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf $@

tests/test_dsp: tests/test_dsp.c src/dsp.c src/wav.c src/dsp.h src/wav.h
	gcc -std=gnu11 -O1 -g -Wall -Wextra -fsanitize=address,undefined -o $@ tests/test_dsp.c src/dsp.c src/wav.c -lm

test: tests/test_dsp
	./tests/test_dsp

test-e2e: $(TARGET)
	./tests/e2e.sh

package: $(TARGET)
	./tools/package.sh $(VERSION)

clean:
	rm -rf $(TARGET) tests/test_dsp dist build

.PHONY: all test test-e2e package clean
