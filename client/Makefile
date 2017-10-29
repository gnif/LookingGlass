CFLAGS=-g -Og -std=gnu99 -march=native -Wall -Werror -I./ -DDEBUG
LDFLAGS=-lrt -lGL

CFLAGS+=`pkg-config --cflags sdl2`
LDFLAGS+=`pkg-config --libs sdl2`

CFLAGS+=`pkg-config --cflags libssl openssl`
LDFLAGS+=`pkg-config --libs libssl openssl`

CFLAGS+=`pkg-config --cflags spice-protocol`

all:
	gcc ${CFLAGS} -o bin/main main.c spice/spice.c ivshmem/ivshmem.c ${LDFLAGS}
