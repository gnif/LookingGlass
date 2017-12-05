BINARY  = looking-glass-client
CFLAGS  = -g -Og -std=gnu99 -march=native -Wall -Werror -I./ -I../common -DDEBUG
LDFLAGS = -lrt

LIBS    = sdl2 SDL2_ttf gl glu libssl openssl spice-protocol
CFLAGS  += $(shell pkg-config --cflags $(LIBS))
LDFLAGS += $(shell pkg-config --libs   $(LIBS))
BUILD   ?= .build
BIN     ?= bin

OBJS	= main.o \
	  spice/spice.o \
	  ivshmem/ivshmem.o

BUILD_OBJS = $(foreach obj,$(OBJS),$(BUILD)/$(obj))

all: $(BINARY)

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	gcc -c $(CFLAGS) -o $@ $< $(LDFLAGS)

$(BINARY): $(BUILD_OBJS)
	@mkdir -p $(dir $(BIN)/$@)
	gcc $(CFLAGS) -o $(BIN)/$(BINARY) $(BUILD_OBJS) $(LDFLAGS)

clean:
	rm -rf $(BUILD) $(BIN)

.PHONY: clean
