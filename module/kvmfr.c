/*
UIO KVMFR Driver
Copyright (C) 2017 Geoffrey McRae <geoff@hostfission.com>
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

#include <asm/io.h>

static int kvmfr_pci_probe(struct pci_dev *dev, const struct pci_device_id *id)
{
  struct uio_info *info;

  info = kzalloc(sizeof(struct uio_info), GFP_KERNEL);
  if (!info)
    return -ENOMEM;

  if (pci_enable_device(dev))
    goto out_free;

  if (pci_request_regions(dev, "kvmfr"))
    goto out_disable;

  info->mem[0].addr = pci_resource_start(dev, 2);
  if (!info->mem[0].addr)
    goto out_release;

  info->mem[0].internal_addr = ioremap_wt(
    pci_resource_start(dev, 2),
    pci_resource_len  (dev, 2)
  );
  if (!info->mem[0].internal_addr)
    goto out_release;

  info->mem[0].size    = pci_resource_len(dev, 2);
  info->mem[0].memtype = UIO_MEM_PHYS;
  info->name           = "KVMFR";
  info->version        = "0.0.1";
  info->irq            = 0;
  info->irq_flags      = 0;
  info->handler        = NULL;

  if (uio_register_device(&dev->dev, info))
    goto out_unmap;

  pci_set_drvdata(dev, info);
  return 0;

out_unmap:
  printk("unmap\n");
  iounmap(info->mem[0].internal_addr);
out_release:
  printk("release\n");
  pci_release_regions(dev);
out_disable:
  printk("disable\n");
  pci_disable_device(dev);
out_free:
  printk("free\n");
  kfree(info);
  return -ENODEV;
}

static void kvmfr_pci_remove(struct pci_dev *dev)
{
  struct uio_info *info = pci_get_drvdata(dev);

  uio_unregister_device(info);
  pci_release_regions(dev);
  pci_disable_device(dev);
  iounmap(info->mem[0].internal_addr);

  kfree(info);
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
  .name     = "kvmfr",
  .id_table = kvmfr_pci_ids,
  .probe    = kvmfr_pci_probe,
  .remove   = kvmfr_pci_remove
};

module_pci_driver(kvmfr_pci_driver);
MODULE_DEVICE_TABLE(pci, kvmfr_pci_ids);
MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Geoffrey McRae");