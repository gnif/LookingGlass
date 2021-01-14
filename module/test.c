#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <sys/mman.h>

#include "kvmfr.h"

int main(void)
{
  int fd = open("/dev/kvmfr0", O_RDWR);
  if (fd < 0)
  {
    perror("open");
    return -1;
  }

  unsigned long size      = ioctl(fd, KVMFR_DMABUF_GETSIZE , 0);
  printf("Size: %lu MiB\n", size / 1024 / 1024);

  struct kvmfr_dmabuf_create create =
  {
    .flags  = KVMFR_DMABUF_FLAG_CLOEXEC,
    .offset = 0x0,
    .size   = size,
  };
  int dmaFd = ioctl(fd, KVMFR_DMABUF_CREATE, &create);
  if (dmaFd < 0)
  {
    perror("ioctl");
    return -1;
  }

  void * mem = mmap(NULL, create.size, PROT_READ | PROT_WRITE, MAP_SHARED, dmaFd, 0);
  if (!mem)
  {
    perror("mmap");
    return -1;
  }

  memset(mem, 0xAA, create.size);
  munmap(mem, create.size);
  close(fd);
  return 0;
}