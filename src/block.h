// SPDX-License-Identifier: LGPL-2.1+

#ifndef EDISKSD_BLOCK_H
#define EDISKSD_BLOCK_H

struct udev_device;

struct block_device;
struct filesystem;

struct block_device *block_device_lookup_by_dev(dev_t dev);
char *block_device_get_path(const struct block_device *block);
dev_t block_device_get_dev(const struct block_device *block);

int block_init(void);
void block_exit(void);

void block_added(struct udev_device *);
void block_removed(struct udev_device *);
void block_changed(struct udev_device *);

#endif
