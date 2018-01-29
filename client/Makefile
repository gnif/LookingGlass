BINARY  = looking-glass-client
CFLAGS  = -g -O3 -std=gnu99 -march=native -Wall -Werror -I./ -I../common -DDEBUG -DATOMIC_LOCKING
LDFLAGS = -lrt

CFLAGS  += -ffast-math
CFLAGS  += -fdata-sections -ffunction-sections
LDFLAGS += -Wl,--gc-sections
CFLAGS  += -Wfatal-errors

LIBS    = sdl2 SDL2_ttf gl glu libssl openssl spice-protocol fontconfig x11 libconfig libva libva-glx
CFLAGS  += $(shell pkg-config --cflags $(LIBS))
LDFLAGS += $(shell pkg-config --libs   $(LIBS))
BUILD   ?= .build
BIN     ?= bin

CFLAGS  += -DBUILD_VERSION='"$(shell git describe --always --long --dirty --abbrev=10 --tags)"'

OBJS	= main.o \
	  lg-renderer.o \
	  spice/spice.o \
	  parsers/nal.o \
	  decoders/null.o \
	  decoders/h264.o \
	  renderers/opengl.o
#	  renderers/opengl-basic.o

BUILD_OBJS = $(foreach obj,$(OBJS),$(BUILD)/$(obj))

all: $(BIN)/$(BINARY) $(BIN)/xlib-shim.so

$(BIN)/xlib-shim.so:
	@mkdir -p $(dir $@)
	$(CC) -fPIC $(CFLAGS) -shared -o $@ xlib-shim.c

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) -c $(CFLAGS) -o $@ $<

$(BIN)/$(BINARY): $(BUILD_OBJS)
	@mkdir -p $(dir $@)
	$(CC) -o $@ $(BUILD_OBJS) $(LDFLAGS)

clean:
	rm -rf $(BUILD) $(BIN)

.PHONY: clean
