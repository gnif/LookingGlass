/*
UIO KVMFR Driver
Copyright (C) 2017-2019 Geoffrey McRae <geoff@hostfission.com>
https://looking-glass.hostfission.com

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; either version 2 of the License, or (at your option) any later
version.

This program is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc., 59 Temple
Place, Suite 330, Boston, MA 02111-1307 USA
*/

#define PCI_KVMFR_VENDOR_ID 0x1af4 //Red Hat Inc,
#define PCI_KVMFR_DEVICE_ID 0x1110 //Inter-VM shared memory

#include <linux/device.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/uio_driver.h>
#include <linux/fs.h>
#include <linux/dma-buf.h>
#include <linux/highmem.h>
#include <linux/version.h>

#include <asm/io.h>

#include "kvmfr.h"

DEFINE_MUTEX(minor_lock);
DEFINE_IDR(kvmfr_idr);

#define KVMFR_UIO_NAME    "KVMFR"
#define KVMFR_UIO_VER     "0.0.2"
#define KVMFR_DEV_NAME    "kvmfr"
#define KVMFR_MAX_DEVICES 10

struct kvmfr_info
{
  int             major;
  struct class  * pClass;
};

static struct kvmfr_info *kvmfr;

struct kvmfr_dev
{
  unsigned long        size;
  struct uio_info      uio;
  int                  minor;
  dev_t                devNo;
  struct device      * pDev;
  struct dev_pagemap   pgmap;
  void               * addr;
};

struct kvmfrbuf
{
  struct kvmfr_dev    * kdev;
  pgoff_t               pagecount;
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
  if ((vma->vm_flags & (VM_SHARED | VM_MAYSHARE)) == 0)
    return -EINVAL;
  vma->vm_ops          = &kvmfr_vm_ops;
  vma->vm_private_data = buf->priv;
  return 0;
}

static const struct dma_buf_ops kvmfrbuf_ops =
{
  .map_dma_buf   = map_kvmfrbuf,
  .unmap_dma_buf = unmap_kvmfrbuf,
  .release       = release_kvmfrbuf,
  .mmap          = mmap_kvmfrbuf
};

inline static unsigned long get_min_align(void)
{
  if (IS_ENABLED(CONFIG_SPARSEMEM_VMEMMAP))
    return PAGES_PER_SUBSECTION;
  else
    return PAGES_PER_SECTION;
}

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

  if (create.offset + create.size > kdev->size)
    return -EINVAL;

  kbuf = kzalloc(sizeof(struct kvmfrbuf), GFP_KERNEL);
  if (!kbuf)
    return -ENOMEM;

  kbuf->kdev      = kdev;
  kbuf->pagecount = create.size >> PAGE_SHIFT;
  kbuf->pages     = kmalloc_array(kbuf->pagecount, sizeof(*kbuf->pages), GFP_KERNEL);
  if (!kbuf->pages)
  {
    ret = -ENOMEM;
    goto err;
  }

  p = ((u8*)kdev->addr) + create.offset;
  for(i = 0; i < kbuf->pagecount; ++i)
  {
    kbuf->pages[i] = virt_to_page(p);
    p += PAGE_SIZE;
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

static struct file_operations fops =
{
  .owner          = THIS_MODULE,
  .unlocked_ioctl = device_ioctl
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

  kdev->uio.mem[0].addr = pci_resource_start(dev, 2);
  if (!kdev->uio.mem[0].addr)
    goto out_release;

  kdev->uio.mem[0].internal_addr = ioremap_wt(
    pci_resource_start(dev, 2),
    pci_resource_len  (dev, 2)
  );

  if (!kdev->uio.mem[0].internal_addr)
    goto out_release;

  kdev->size = pci_resource_len(dev, 2);

  kdev->uio.mem[0].size    = pci_resource_len(dev, 2);
  kdev->uio.mem[0].memtype = UIO_MEM_PHYS;
  kdev->uio.name           = KVMFR_UIO_NAME;
  kdev->uio.version        = KVMFR_UIO_VER;
  kdev->uio.irq            = 0;
  kdev->uio.irq_flags      = 0;
  kdev->uio.handler        = NULL;

  if (uio_register_device(&dev->dev, &kdev->uio))
    goto out_unmap;

  mutex_lock(&minor_lock);
  kdev->minor = idr_alloc(&kvmfr_idr, kdev, 0, KVMFR_MAX_DEVICES, GFP_KERNEL);
  if (kdev->minor < 0)
  {
    mutex_unlock(&minor_lock);
    goto out_unreg;
  }
  mutex_unlock(&minor_lock);

  kdev->devNo = MKDEV(kvmfr->major, kdev->minor);
  kdev->pDev  = device_create(kvmfr->pClass, NULL, kdev->devNo, NULL, KVMFR_DEV_NAME "%d", kdev->minor);
  if (IS_ERR(kdev->pDev))
    goto out_unminor;

  kdev->pgmap.res.start = pci_resource_start(dev, 2);
  kdev->pgmap.res.end   = pci_resource_end  (dev, 2);
  kdev->pgmap.res.flags = pci_resource_flags(dev, 2);
  kdev->pgmap.type      = MEMORY_DEVICE_DEVDAX;

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
out_unreg:
  uio_unregister_device(&kdev->uio);
out_unmap:
  iounmap(kdev->uio.mem[0].internal_addr);
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

  uio_unregister_device(&kdev->uio);
  pci_release_regions(dev);
  pci_disable_device(dev);
  iounmap(kdev->uio.mem[0].internal_addr);

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

  ret = pci_register_driver(&kvmfr_pci_driver);
  if (ret < 0)
    goto out_class_destroy;

  return 0;

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
  class_destroy(kvmfr->pClass);
  unregister_chrdev(kvmfr->major, KVMFR_DEV_NAME);
  kfree(kvmfr);
  kvmfr = NULL;
}

module_init(kvmfr_module_init);
module_exit(kvmfr_module_exit);

MODULE_DEVICE_TABLE(pci, kvmfr_pci_ids);
MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Geoffrey McRae");
