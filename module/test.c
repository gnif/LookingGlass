/**
 * Looking Glass
 * Copyright © 2017-2021 The Looking Glass Authors
 * https://looking-glass.io
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc., 59
 * Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */

#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
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
  int page_size = getpagesize();

  int fd = open("/dev/kvmfr0", O_RDWR);
  if (fd < 0)
  {
    perror("open");
    return -1;
  }

  unsigned long size      = ioctl(fd, KVMFR_DMABUF_GETSIZE , 0);
  printf("Size: %lu MiB\n", size / 1024 / 1024);

  // mmaping 0-offset dmabuf with 0 offset
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
  if (mem == MAP_FAILED)
  {
    perror("mmap on dmabuf with no offset");
    return -1;
  }
  memset(mem, 0xAA, create.size);
  strcpy(mem + page_size, "Hello, world!");
  munmap(mem, create.size);

  // mmaping 0-offset dmabuf with 1 page offset
  mem = mmap(NULL, page_size, PROT_READ | PROT_WRITE, MAP_SHARED, dmaFd, page_size);
  if (mem == MAP_FAILED)
  {
    perror("mmap on dmabuf with offset");
    return -1;
  }
  printf("Read string: %s\n", (char *) mem);
  munmap(mem, page_size);

  close(dmaFd);

  // mmaping page-offset dmabuf with 0 offset
  create.offset = page_size;
  create.size   = 2 * page_size;
  dmaFd = ioctl(fd, KVMFR_DMABUF_CREATE, &create);
  mem = mmap(NULL, create.size, PROT_READ | PROT_WRITE, MAP_SHARED, dmaFd, 0);
  if (mem == MAP_FAILED)
  {
    perror("mmap on offset dmabuf with no offset");
    return -1;
  }
  printf("Read string: %s\n", (char *) mem);
  munmap(mem, create.size);

  // mmaping page-offset dmabuf with 1 page offset
  char *bytes = mmap(NULL, page_size, PROT_READ | PROT_WRITE, MAP_SHARED, dmaFd, page_size);
  if (bytes == MAP_FAILED)
  {
    perror("mmap on offset dmabuf with offset");
    return -1;
  }
  for (int i = 0; i < page_size; i++)
    if (bytes[i] != (char) 0xAA)
      printf("Index: %d: 0x%02x\n", i, (unsigned) bytes[i]);
  munmap(mem, page_size);

  close(dmaFd);

  // mmaping device with 0 offset
  mem = mmap(NULL, page_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, page_size);
  if (mem == MAP_FAILED)
  {
    perror("mmap on file with offset");
    return -1;
  }
  printf("Read string: %s\n", (char *) mem);
  munmap(mem, page_size);

  // mmaping device with 0 offset
  uint32_t *data = mmap(NULL, create.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (data == MAP_FAILED)
  {
    perror("mmap on file with no offset");
    return -1;
  }
  for (size_t i = 0; i < create.size / sizeof(uint32_t); i++)
  {
    if (data[i] != 0xAAAAAAAA)
      printf("Index %lu: 0x%08" PRIx32 "\n", i, data[i]);
  }
  munmap(data, create.size);

  close(fd);
  return 0;
}
