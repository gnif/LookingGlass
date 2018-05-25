BINARY  = looking-glass-host.exe
CFLAGS  = -g -O3 -march=native -Wall -Werror -I./ -I../common # -DDEBUG
LDFLAGS = -lshlwapi -ldxgi -ld3d11 -lsetupapi -luuid -lole32 -lmfplat -lmfuuid

CFLAGS  += -ffast-math
CFLAGS  += -fdata-sections -ffunction-sections
CFLAGS  += -I../ -I.
LDFLAGS += -Wl,--gc-sections -mwindows
CFLAGS  += -DWINVER=0x0602 -DUNICODE

PREFIX ?= x86_64-w64-mingw32-
STRIP  = $(PREFIX)strip
CC     = $(PREFIX)cc
CXX    = $(PREFIX)c++
LD     = $(CXX)

BUILD  ?= .build
BIN    ?= bin

CFLAGS  += -DBUILD_VERSION='"$(shell git describe --always --long --dirty --abbrev=10 --tags)"'

OBJS	= main.o \
	  CrashHandler.o \
	  MultiMemcpy.o \
	  IVSHMEM.o \
	  Service.o \
	  Capture/DXGI.o

ifeq ($(ENABLE_TRACING),1)
CFLAGS  += -DENABLE_TRACING
OBJS    += TraceUtil.o
endif

ifeq ($(CONFIG_CAPTURE_NVFBC),1)
CFLAGS  += -DCONFIG_CAPTURE_NVFBC=1 -I../vendor
OBJS    += Capture/NvFBC.o
endif

BUILD_OBJS = $(foreach obj,$(OBJS),$(BUILD)/$(obj))

all: $(BIN)/$(BINARY)

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) -c $(CFLAGS) -o $@ $<

$(BUILD)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) -c $(CFLAGS) -o $@ $<

$(BIN)/$(BINARY): $(BUILD_OBJS)
	@mkdir -p $(dir $@)
	$(LD) -o $@ $^ $(LDFLAGS)
	$(STRIP) -s $@

clean:
	rm -rf $(BUILD) $(BIN)

.PHONY: clean
