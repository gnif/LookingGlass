USE_DXGI ?= 1

CC=gcc.exe

CFLAGS  = -std=gnu99 -Wall -Werror
CFLAGS += -g -O0
CFLAGS += -I.
CFLAGS += -I../common

OBJS  = app.o

# if windows
ifdef OS
  LDFLAGS = -L./dll
  LIBS   += -lsetupapi
  OBJS   += windows/platform.o
  OBJS   += windows/windebug.o
  CFLAGS += -I../vendor/kvm-guest-drivers-windows

  ifeq ($(USE_DXGI), 1)
    CFLAGS += -DUSE_DXGI -DCOBJMACROS -DINITGUID
    LIBS   += -ld3d11 -ldxgi
    DLLS   += dll/libd3d11.a
    OBJS   += capture/dxgi.o
  endif
endif

all: $(OBJS) $(DLLS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o looking-glass-host.exe $(OBJS) $(LIBS)

clean:
	rm -v $(OBJS) $(DLLS) looking-glass-host.exe

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

%.a: %.def
	dlltool --def $< --output-lib $@
