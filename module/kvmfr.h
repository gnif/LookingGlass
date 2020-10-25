#ifndef _KVMFR_H
#define _KVMFR_H

#include <linux/types.h>
#include <linux/ioctl.h>

#define KVMFR_DMABUF_FLAG_CLOEXEC 0x1

struct kvmfr_dmabuf_create {
  __u8  flags;
  __u64 offset;
  __u64 size;
};

#define KVMFR_DMABUF_GETSIZE _IO('u', 0x44)
#define KVMFR_DMABUF_CREATE  _IOW('u', 0x42, struct kvmfr_dmabuf_create)

#endif