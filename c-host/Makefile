USE_DXGI ?= 1

CC=gcc.exe

OUTPUT = looking-glass-host
EXT    =

CFLAGS  = -std=gnu99 -Wall -Werror
CFLAGS += -g -O0
CFLAGS += -I.
CFLAGS += -I../common

OBJS  = app.o

# if windows
ifdef OS
  LDFLAGS = -L./windows/dll
  LIBS   += -lsetupapi
  OBJS   += windows/platform.o
  OBJS   += windows/windebug.o
  CFLAGS += -I../vendor/kvm-guest-drivers-windows
  EXT     = .exe

  ifeq ($(USE_DXGI), 1)
    CFLAGS += -DUSE_DXGI -DCOBJMACROS -DINITGUID
    LIBS   += -ld3d11 -ldxgi
    DLLS   += windows/dll/libd3d11.a
    OBJS   += windows/capture/dxgi.o
  endif
else
  OBJS += linux/platform.o
endif

all: $(OBJS) $(DLLS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $(OUTPUT)$(EXT) $(OBJS) $(LIBS)

clean:
	rm -v $(OBJS) $(DLLS) $(OUTPUT)$(EXT)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

%.a: %.def
	dlltool --def $< --output-lib $@
