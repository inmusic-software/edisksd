// SPDX-License-Identifier: LGPL-2.1+

#ifndef EDISKSD_FILESYSTEM_H
#define EDISKSD_FILESYSTEM_H

struct block_device;
struct udev_device;

int filesystem_init(void);
void filesystem_exit(void);

void filesystem_added(struct block_device *, struct udev_device *);
void filesystem_removed(struct block_device *);
void filesystem_changed(struct block_device *, struct udev_device *);

void filesystem_mounted(dev_t dev, const char *mountpoint);
void filesystem_unmounted(dev_t dev, const char *mountpoint);

#endif
