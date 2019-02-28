USE_DXGI ?= 1

CC=gcc.exe

CFLAGS  = -std=gnu99 -Wall -Werror
CFLAGS += -g -O0
CFLAGS += -DCOBJMACROS
CFLAGS += -DINITGUID
CFLAGS += -I.
CFLAGS += -I../common

OBJS  = main.o

# if windows
ifdef OS
  LDFLAGS = -L./dll
  OBJS += windebug.o

  ifeq ($(USE_DXGI), 1)
    CFLAGS += -DUSE_DXGI
    LIBS   += -ld3d11
    LIBS   += -ldxgi
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
