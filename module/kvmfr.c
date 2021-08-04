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

#define PCI_KVMFR_VENDOR_ID 0x1af4 //Red Hat Inc,
#define PCI_KVMFR_DEVICE_ID 0x1110 //Inter-VM shared memory

#include <linux/device.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/dma-buf.h>
#include <linux/highmem.h>
#include <linux/version.h>

#include <asm/io.h>

#include "kvmfr.h"

DEFINE_MUTEX(minor_lock);
DEFINE_IDR(kvmfr_idr);

#define KVMFR_DEV_NAME    "kvmfr"
#define KVMFR_MAX_DEVICES 10

static int static_size_mb[KVMFR_MAX_DEVICES];
static int static_count;
module_param_array(static_size_mb, int, &static_count, 0000);
MODULE_PARM_DESC(static_size_mb, "List of static devices to create in MiB");

struct kvmfr_info
{
  int             major;
  struct class  * pClass;
};

static struct kvmfr_info *kvmfr;

enum kvmfr_type
{
  KVMFR_TYPE_PCI,
  KVMFR_TYPE_STATIC,
};

struct kvmfr_dev
{
  unsigned long        size;
  int                  minor;
  dev_t                devNo;
  struct device      * pDev;
  struct dev_pagemap   pgmap;
  void               * addr;
  enum kvmfr_type      type;
};

struct kvmfrbuf
{
  struct kvmfr_dev    * kdev;
  pgoff_t               pagecount;
  unsigned long         offset;
  struct page        ** pages;
};

static vm_fault_t kvmfr_vm_fault(struct vm_fault *vmf)
{
  struct vm_area_struct *vma = vmf->vma;
  struct kvmfrbuf *kbuf = (struct kvmfrbuf *)vma->vm_private_data;

  vmf->page = kbuf->pages[vmf->pgoff];
  get_page(vmf->page);
  return 0;
}

static const struct vm_operations_struct kvmfr_vm_ops =
{
  .fault = kvmfr_vm_fault
};

static struct sg_table * map_kvmfrbuf(struct dma_buf_attachment *at,
    enum dma_data_direction direction)
{
  struct kvmfrbuf *kbuf = at->dmabuf->priv;
  struct sg_table *sg;
  int ret;

  sg = kzalloc(sizeof(*sg), GFP_KERNEL);
  if (!sg)
    return ERR_PTR(-ENOMEM);

  ret = sg_alloc_table_from_pages(sg, kbuf->pages, kbuf->pagecount,
      0, kbuf->pagecount << PAGE_SHIFT, GFP_KERNEL);
  if (ret < 0)
    goto err;

  if (!dma_map_sg(at->dev, sg->sgl, sg->nents, direction))
  {
    ret = -EINVAL;
    goto err;
  }

  return sg;

err:
  sg_free_table(sg);
  kfree(sg);
  return ERR_PTR(ret);
}

static void unmap_kvmfrbuf(struct dma_buf_attachment * at, struct sg_table * sg, enum dma_data_direction direction)
{
  dma_unmap_sg(at->dev, sg->sgl, sg->nents, direction);
  sg_free_table(sg);
  kfree(sg);
}

static void release_kvmfrbuf(struct dma_buf * buf)
{
  struct kvmfrbuf *kbuf = (struct kvmfrbuf *)buf->priv;
  kfree(kbuf->pages);
  kfree(kbuf);
}

static int mmap_kvmfrbuf(struct dma_buf * buf, struct vm_area_struct * vma)
{
  struct kvmfrbuf * kbuf = (struct kvmfrbuf *)buf->priv;
  unsigned long size = vma->vm_end - vma->vm_start;
  unsigned long offset = vma->vm_pgoff << PAGE_SHIFT;

  if ((offset + size > (kbuf->pagecount << PAGE_SHIFT)) || (offset + size < offset))
    return -EINVAL;

  if ((vma->vm_flags & (VM_SHARED | VM_MAYSHARE)) == 0)
    return -EINVAL;

  switch (kbuf->kdev->type)
  {
    case KVMFR_TYPE_PCI:
      vma->vm_ops          = &kvmfr_vm_ops;
      vma->vm_private_data = buf->priv;
      return 0;

    case KVMFR_TYPE_STATIC:
      return remap_vmalloc_range(vma, kbuf->kdev->addr + kbuf->offset, vma->vm_pgoff);

    default:
      return -EINVAL;
  }
}

static const struct dma_buf_ops kvmfrbuf_ops =
{
  .map_dma_buf   = map_kvmfrbuf,
  .unmap_dma_buf = unmap_kvmfrbuf,
  .release       = release_kvmfrbuf,
  .mmap          = mmap_kvmfrbuf
};

static long kvmfr_dmabuf_create(struct kvmfr_dev * kdev, struct file * filp, unsigned long arg)
{
  struct kvmfr_dmabuf_create create;
  DEFINE_DMA_BUF_EXPORT_INFO(exp_kdev);
  struct kvmfrbuf * kbuf;
  struct dma_buf  * buf;
  u32 i;
  u8 *p;
  int ret = -EINVAL;

  if (copy_from_user(&create, (void __user *)arg,
        sizeof(create)))
      return -EFAULT;

  if (!IS_ALIGNED(create.offset, PAGE_SIZE) ||
      !IS_ALIGNED(create.size  , PAGE_SIZE))
  {
    printk("kvmfr: buffer not aligned to 0x%lx bytes", PAGE_SIZE);
    return -EINVAL;
  }

  if ((create.offset + create.size > kdev->size) || (create.offset + create.size < create.offset))
    return -EINVAL;

  kbuf = kzalloc(sizeof(struct kvmfrbuf), GFP_KERNEL);
  if (!kbuf)
    return -ENOMEM;

  kbuf->kdev      = kdev;
  kbuf->pagecount = create.size >> PAGE_SHIFT;
  kbuf->offset    = create.offset;
  kbuf->pages     = kmalloc_array(kbuf->pagecount, sizeof(*kbuf->pages), GFP_KERNEL);
  if (!kbuf->pages)
  {
    ret = -ENOMEM;
    goto err;
  }

  p = ((u8*)kdev->addr) + create.offset;

  switch (kdev->type)
  {
    case KVMFR_TYPE_PCI:
      for (i = 0; i < kbuf->pagecount; ++i)
      {
        kbuf->pages[i] = virt_to_page(p);
        p += PAGE_SIZE;
      }
      break;

    case KVMFR_TYPE_STATIC:
      for (i = 0; i < kbuf->pagecount; ++i)
      {
        kbuf->pages[i] = vmalloc_to_page(p);
        p += PAGE_SIZE;
      }
      break;
  }

  exp_kdev.ops   = &kvmfrbuf_ops;
  exp_kdev.size  = create.size;
  exp_kdev.priv  = kbuf;
  exp_kdev.flags = O_RDWR;

  buf = dma_buf_export(&exp_kdev);
  if (IS_ERR(buf))
  {
    ret = PTR_ERR(buf);
    goto err;
  }

  printk("kvmfr_dmabuf_create: offset: %llu, size: %llu\n", create.offset, create.size);
  return dma_buf_fd(buf, create.flags & KVMFR_DMABUF_FLAG_CLOEXEC ? O_CLOEXEC : 0);

err:
  kfree(kbuf->pages);
  kfree(kbuf);
  return ret;
}

static long device_ioctl(struct file * filp, unsigned int ioctl, unsigned long arg)
{
  struct kvmfr_dev * kdev;
  long ret;

  kdev = (struct kvmfr_dev *)idr_find(&kvmfr_idr, iminor(filp->f_inode));
  if (!kdev)
    return -EINVAL;

  switch(ioctl)
  {
    case KVMFR_DMABUF_CREATE:
      ret = kvmfr_dmabuf_create(kdev, filp, arg);
      break;

    case KVMFR_DMABUF_GETSIZE:
      ret = kdev->size;
      break;

    default:
      return -ENOTTY;
  }

  return ret;
}

static vm_fault_t pci_mmap_fault(struct vm_fault *vmf)
{
  struct vm_area_struct * vma = vmf->vma;
  struct kvmfr_dev * kdev = (struct kvmfr_dev *)vma->vm_private_data;

  vmf->page = virt_to_page(kdev->addr + (vmf->pgoff << PAGE_SHIFT));
  get_page(vmf->page);
  return 0;
}

static const struct vm_operations_struct pci_mmap_ops =
{
  .fault = pci_mmap_fault
};

static int device_mmap(struct file * filp, struct vm_area_struct * vma)
{
  struct kvmfr_dev * kdev;
  unsigned long size = vma->vm_end - vma->vm_start;
  unsigned long offset = vma->vm_pgoff << PAGE_SHIFT;

  kdev = (struct kvmfr_dev *)idr_find(&kvmfr_idr, iminor(filp->f_inode));
  if (!kdev)
    return -EINVAL;

  if ((offset + size > kdev->size) || (offset + size < offset))
    return -EINVAL;

  printk(KERN_INFO "mmap kvmfr%d: %lx-%lx with size %lu offset %lu\n",
      kdev->minor, vma->vm_start, vma->vm_end, size, offset);

  switch (kdev->type)
  {
    case KVMFR_TYPE_PCI:
      vma->vm_ops          = &pci_mmap_ops;
      vma->vm_private_data = kdev;
      return 0;

    case KVMFR_TYPE_STATIC:
      return remap_vmalloc_range(vma, kdev->addr, vma->vm_pgoff);

    default:
      return -ENODEV;
  }
}

static struct file_operations fops =
{
  .owner          = THIS_MODULE,
  .unlocked_ioctl = device_ioctl,
  .mmap           = device_mmap,
};

static int kvmfr_pci_probe(struct pci_dev *dev, const struct pci_device_id *id)
{
  struct kvmfr_dev *kdev;

  kdev = kzalloc(sizeof(struct kvmfr_dev), GFP_KERNEL);
  if (!kdev)
    return -ENOMEM;

  if (pci_enable_device(dev))
    goto out_free;

  if (pci_request_regions(dev, KVMFR_DEV_NAME))
    goto out_disable;

  kdev->size = pci_resource_len(dev, 2);
  kdev->type = KVMFR_TYPE_PCI;

  mutex_lock(&minor_lock);
  kdev->minor = idr_alloc(&kvmfr_idr, kdev, 0, KVMFR_MAX_DEVICES, GFP_KERNEL);
  if (kdev->minor < 0)
  {
    mutex_unlock(&minor_lock);
    goto out_release;
  }
  mutex_unlock(&minor_lock);

  kdev->devNo = MKDEV(kvmfr->major, kdev->minor);
  kdev->pDev  = device_create(kvmfr->pClass, NULL, kdev->devNo, NULL, KVMFR_DEV_NAME "%d", kdev->minor);
  if (IS_ERR(kdev->pDev))
    goto out_unminor;

#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 10, 0)
  kdev->pgmap.res.start = pci_resource_start(dev, 2);
  kdev->pgmap.res.end   = pci_resource_end  (dev, 2);
  kdev->pgmap.res.flags = pci_resource_flags(dev, 2);
#else
  kdev->pgmap.range.start = pci_resource_start(dev, 2);
  kdev->pgmap.range.end   = pci_resource_end  (dev, 2);
  kdev->pgmap.nr_range    = 1;
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 9, 0)
  kdev->pgmap.type = MEMORY_DEVICE_DEVDAX;
#else
  kdev->pgmap.type = MEMORY_DEVICE_GENERIC;
#endif

  kdev->addr = devm_memremap_pages(&dev->dev, &kdev->pgmap);
  if (IS_ERR(kdev->addr))
    goto out_destroy;

  pci_set_drvdata(dev, kdev);
  return 0;

out_destroy:
  device_destroy(kvmfr->pClass, kdev->devNo);
out_unminor:
  mutex_lock(&minor_lock);
  idr_remove(&kvmfr_idr, kdev->minor);
  mutex_unlock(&minor_lock);
out_release:
  pci_release_regions(dev);
out_disable:
  pci_disable_device(dev);
out_free:
  kfree(kdev);
  return -ENODEV;
}

static void kvmfr_pci_remove(struct pci_dev *dev)
{
  struct kvmfr_dev *kdev = pci_get_drvdata(dev);

  devm_memunmap_pages(&dev->dev, &kdev->pgmap);
  device_destroy(kvmfr->pClass, kdev->devNo);

  mutex_lock(&minor_lock);
  idr_remove(&kvmfr_idr, kdev->minor);
  mutex_unlock(&minor_lock);

  pci_release_regions(dev);
  pci_disable_device(dev);

  kfree(kdev);
}

static struct pci_device_id kvmfr_pci_ids[] =
{
  {
    .vendor    = PCI_KVMFR_VENDOR_ID,
    .device    = PCI_KVMFR_DEVICE_ID,
    .subvendor = PCI_ANY_ID,
    .subdevice = PCI_ANY_ID
  },
  { 0, }
};

static struct pci_driver kvmfr_pci_driver =
{
  .name     = KVMFR_DEV_NAME,
  .id_table = kvmfr_pci_ids,
  .probe    = kvmfr_pci_probe,
  .remove   = kvmfr_pci_remove
};

static int create_static_device_unlocked(int size_mb)
{
  struct kvmfr_dev * kdev;
  int ret = -ENODEV;

  kdev = kzalloc(sizeof(struct kvmfr_dev), GFP_KERNEL);
  if (!kdev)
    return -ENOMEM;

  kdev->size = size_mb * 1024 * 1024;
  kdev->type = KVMFR_TYPE_STATIC;
  kdev->addr = vmalloc_user(kdev->size);
  if (!kdev->addr)
  {
    printk(KERN_ERR "kvmfr: failed to allocate memory for static device: %d MiB\n", size_mb);
    ret = -ENOMEM;
    goto out_free;
  }

  kdev->minor = idr_alloc(&kvmfr_idr, kdev, 0, KVMFR_MAX_DEVICES, GFP_KERNEL);
  if (kdev->minor < 0)
    goto out_release;

  kdev->devNo = MKDEV(kvmfr->major, kdev->minor);
  kdev->pDev  = device_create(kvmfr->pClass, NULL, kdev->devNo, NULL, KVMFR_DEV_NAME "%d", kdev->minor);
  if (IS_ERR(kdev->pDev))
    goto out_unminor;

  return 0;

out_unminor:
  idr_remove(&kvmfr_idr, kdev->minor);
out_release:
  vfree(kdev->addr);
out_free:
  kfree(kdev);
  return ret;
}

static void free_static_device_unlocked(struct kvmfr_dev * kdev)
{
  device_destroy(kvmfr->pClass, kdev->devNo);
  idr_remove(&kvmfr_idr, kdev->minor);
  vfree(kdev->addr);
  kfree(kdev);
}

static void free_static_devices(void)
{
  int id;
  struct kvmfr_dev * kdev;

  mutex_lock(&minor_lock);
  idr_for_each_entry(&kvmfr_idr, kdev, id)
    free_static_device_unlocked(kdev);
  mutex_unlock(&minor_lock);
}

static int create_static_devices(void)
{
  int i;
  int ret = 0;

  mutex_lock(&minor_lock);
  printk(KERN_INFO "kvmfr: creating %d static devices\n", static_count);
  for (i = 0; i < static_count; ++i)
  {
    ret = create_static_device_unlocked(static_size_mb[i]);
    if (ret < 0)
      break;
  }
  mutex_unlock(&minor_lock);

  if (ret < 0)
    free_static_devices();
  return ret;
}

static int __init kvmfr_module_init(void)
{
  int ret;

  kvmfr = kzalloc(sizeof(struct kvmfr_info), GFP_KERNEL);
  if (!kvmfr)
    return -ENOMEM;

  kvmfr->major = register_chrdev(0, KVMFR_DEV_NAME, &fops);
  if (kvmfr->major < 0)
    goto out_free;

  kvmfr->pClass = class_create(THIS_MODULE, KVMFR_DEV_NAME);
  if (IS_ERR(kvmfr->pClass))
    goto out_unreg;

  ret = create_static_devices();
  if (ret < 0)
    goto out_class_destroy;

  ret = pci_register_driver(&kvmfr_pci_driver);
  if (ret < 0)
    goto out_free_static;

  return 0;

out_free_static:
  free_static_devices();
out_class_destroy:
  class_destroy(kvmfr->pClass);
out_unreg:
  unregister_chrdev(kvmfr->major, KVMFR_DEV_NAME);
out_free:
  kfree(kvmfr);

  kvmfr = NULL;
  return -ENODEV;
}

static void __exit kvmfr_module_exit(void)
{
  pci_unregister_driver(&kvmfr_pci_driver);
  free_static_devices();
  class_destroy(kvmfr->pClass);
  unregister_chrdev(kvmfr->major, KVMFR_DEV_NAME);
  kfree(kvmfr);
  kvmfr = NULL;
}

module_init(kvmfr_module_init);
module_exit(kvmfr_module_exit);

MODULE_DEVICE_TABLE(pci, kvmfr_pci_ids);
MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Geoffrey McRae <geoff@hostfission.com>");
MODULE_AUTHOR("Guanzhong Chen <quantum2048@gmail.com>");
MODULE_VERSION("0.0.7");
