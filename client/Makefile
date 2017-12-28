BINARY  = looking-glass-client
CFLAGS  = -g -O3 -std=gnu99 -march=native -Wall -Werror -I./ -I../common -DDEBUG -DATOMIC_LOCKING
LDFLAGS = -lrt

CFLAGS  += -ffast-math
CFLAGS  += -fdata-sections -ffunction-sections
LDFLAGS += -Wl,--gc-sections

LIBS    = sdl2 SDL2_ttf gl glu libssl openssl spice-protocol fontconfig x11
CFLAGS  += $(shell pkg-config --cflags $(LIBS))
LDFLAGS += $(shell pkg-config --libs   $(LIBS))
BUILD   ?= .build
BIN     ?= bin

CFLAGS  += -DBUILD_VERSION='"$(shell git describe --always --long --dirty --abbrev=10 --tags)"'

OBJS	= main.o \
	  lg-renderer.o \
	  spice/spice.o \
	  renderers/opengl.o
#	  renderers/opengl-basic.o

BUILD_OBJS = $(foreach obj,$(OBJS),$(BUILD)/$(obj))

all: $(BIN)/$(BINARY)

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	gcc -c $(CFLAGS) -o $@ $<

$(BIN)/$(BINARY): $(BUILD_OBJS)
	@mkdir -p $(dir $@)
	gcc -o $@ $(BUILD_OBJS) $(LDFLAGS)

clean:
	rm -rf $(BUILD) $(BIN)

.PHONY: clean
