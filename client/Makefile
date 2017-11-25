BINARY=kvmgfx-client
CFLAGS=-g -Og -std=gnu99 -march=native -Wall -Werror -I../common -DDEBUG
LDFLAGS=-lrt

CFLAGS+=`pkg-config --cflags sdl2`
LDFLAGS+=`pkg-config --libs sdl2`

CFLAGS+=`pkg-config --cflags gl`
LDFLAGS+=`pkg-config --libs gl`

CFLAGS+=`pkg-config --cflags glu`
LDFLAGS+=`pkg-config --libs glu`

CFLAGS+=`pkg-config --cflags libssl openssl`
LDFLAGS+=`pkg-config --libs libssl openssl`

CFLAGS+=`pkg-config --cflags spice-protocol`

all:
	mkdir -p bin
	gcc ${CFLAGS} -o bin/${BINARY} main.c spice/spice.c ivshmem/ivshmem.c ${LDFLAGS}
