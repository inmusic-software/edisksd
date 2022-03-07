// SPDX-License-Identifier: LGPL-2.1+

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#include <linux/fs.h>
#include <sys/ioctl.h>

#include <ell/ell.h>
#include <libudev.h>

#include "src/edisksd.h"

#include "src/block.h"
#include "src/dbus.h"
#include "src/filesystem.h"


struct block_device {
	char *syspath;
	char *device;
	char *devpath;

	char **devlinks;

	dev_t dev;
	uint64_t size;

	bool ignore;
	bool ro;
};

static struct l_hashmap *block_hashmap;

struct find_by_dev_ctx {
	struct block_device *block;
	dev_t dev;
};

static void find_by_dev(const void *key, void *value, void *user_data)
{
	struct find_by_dev_ctx *ctx = user_data;
	struct block_device *block = value;

	if (ctx->dev == block->dev)
		ctx->block = block;
}

static bool sysattr_to_bool(const char *value)
{
	if (!value)
		return false;

	return value[0] == '1' || !strncasecmp(value, "true", strlen("true"));
}

static bool get_udev_bool_property(struct udev_device *dev, const char *key)
{
	const char *value;

	value = udev_device_get_property_value(dev, key);
	if (!value)
		return false;

	return value[0] == '1' || !strcasecmp(value, "true");
}

struct block_device *block_device_lookup_by_dev(dev_t dev)
{
	struct find_by_dev_ctx ctx = {
		.dev = dev,
	};

	l_hashmap_foreach(block_hashmap, find_by_dev, &ctx);

	return ctx.block;
}

char *block_device_get_path(const struct block_device *block)
{
	return l_strdup_printf("%s%s", EDISKSD_BASE_PATH, block->device);
}

dev_t block_device_get_dev(const struct block_device *block)
{
	return block->dev;
}

static bool block_property_get_syspath(struct l_dbus *dbus,
				       struct l_dbus_message *message,
				       struct l_dbus_message_builder *builder,
				       void *user_data)
{
	struct block_device *block = user_data;
	const char *s = block->syspath;

	return dbus_message_builder_append_basic_array(builder, 'y',
						       s, strlen(s) + 1);
}

static bool block_property_get_device(struct l_dbus *dbus,
				      struct l_dbus_message *message,
				      struct l_dbus_message_builder *builder,
				      void *user_data)
{
	struct block_device *block = user_data;
	const char *s = block->device;

	return dbus_message_builder_append_basic_array(builder, 'y',
						       s, strlen(s) + 1);
}

static bool block_property_get_devlinks(struct l_dbus *dbus,
					struct l_dbus_message *message,
					struct l_dbus_message_builder *builder,
					void *user_data)
{
	struct block_device *block = user_data;
	char **arr = block->devlinks;
	size_t i;

	if (!l_dbus_message_builder_enter_array(builder, "ay"))
		return false;

	for (i = 0; arr[i]; i++) {
		const char *s = arr[i];
		size_t n = strlen(s) + 1;

		if (!dbus_message_builder_append_basic_array(builder, 'y', s, n))
			return false;
	}

	return l_dbus_message_builder_leave_array(builder);
}

static bool block_property_get_device_number(struct l_dbus *dbus,
					     struct l_dbus_message *message,
					     struct l_dbus_message_builder *builder,
					     void *user_data)
{
	struct block_device *block = user_data;
	uint64_t dev = block->dev;

	return l_dbus_message_builder_append_basic(builder, 't', &dev);
}

static bool block_property_get_hint_ignore(struct l_dbus *dbus,
					   struct l_dbus_message *message,
					   struct l_dbus_message_builder *builder,
					   void *user_data)
{
	struct block_device *block = user_data;
	bool ignore = block->ignore;

	return l_dbus_message_builder_append_basic(builder, 'b', &ignore);
}

static bool block_property_get_ro(struct l_dbus *dbus,
				  struct l_dbus_message *message,
				  struct l_dbus_message_builder *builder,
				  void *user_data)
{
	struct block_device *block = user_data;
	bool ro = block->ro;

	return l_dbus_message_builder_append_basic(builder, 'b', &ro);
}

static bool block_property_get_size(struct l_dbus *dbus,
				    struct l_dbus_message *message,
				    struct l_dbus_message_builder *builder,
				    void *user_data)
{
	struct block_device *block = user_data;
	uint64_t size = block->size;

	return l_dbus_message_builder_append_basic(builder, 't', &size);
}

static void block_register_dbus(struct block_device *block)
{
	L_AUTO_FREE_VAR(char *, path) = block_device_get_path(block);
	struct l_dbus *dbus = dbus_get_bus();

	if (!l_dbus_object_add_interface(dbus, path, L_DBUS_INTERFACE_PROPERTIES,
					 NULL))
		l_warn("Unable to add %s interface", L_DBUS_INTERFACE_PROPERTIES);
	if (!l_dbus_object_add_interface(dbus, path, EDISKSD_BLOCK_INTERFACE,
					 block))
		l_warn("Unable to add %s interface", EDISKSD_BLOCK_INTERFACE);
}

static void setup_block_interface(struct l_dbus_interface *interface)
{
	l_dbus_interface_property(interface, "SysPath", 0, "ay",
				  block_property_get_syspath, NULL);
	l_dbus_interface_property(interface, "Device", 0, "ay",
				  block_property_get_device, NULL);
	l_dbus_interface_property(interface, "DeviceLinks", 0, "aay",
				  block_property_get_devlinks, NULL);
	l_dbus_interface_property(interface, "DeviceNumber", 0, "t",
				  block_property_get_device_number, NULL);
	l_dbus_interface_property(interface, "HintIgnore", 0, "b",
				  block_property_get_hint_ignore, NULL);
	l_dbus_interface_property(interface, "ReadOnly", 0, "b",
				  block_property_get_ro, NULL);
	l_dbus_interface_property(interface, "Size", 0, "t",
				  block_property_get_size, NULL);
}

static void block_device_free(struct block_device *block)
{
	if (!block)
		return;

	l_free(block->syspath);
	l_free(block->device);
	l_free(block->devpath);

	l_strv_free(block->devlinks);

	l_free(block);
}

static bool is_filesystem_device(struct udev_device *device)
{
	const char *usage;

	usage = udev_device_get_property_value(device, "ID_FS_USAGE");

	if (!usage)
		return false;

	return !strcmp(usage, "filesystem");
}

static char **str_array_from_udev_list(struct udev_list_entry *list)
{
	struct udev_list_entry *e;
	size_t n = 0;
	char **arr;

	udev_list_entry_foreach(e, list)
		n++;

	arr = l_new(char *, n + 1);

	n = 0;
	udev_list_entry_foreach(e, list)
		arr[n++] = l_strdup(udev_list_entry_get_name(e));

	arr[n] = NULL;
	return arr;
}

static uint64_t block_get_size(struct block_device *block)
{
	uint64_t val;
	int fd;

	fd = open(block->device, O_CLOEXEC);
	if (fd < 0) {
		l_warn("failed to read block size for %s", block->device);
		return 0;
	}

	if (ioctl(fd, BLKGETSIZE64, &val) < 0) {
		l_warn("failed to read block size for %s", block->device);
		val = 0;
	}

	close(fd);
	return val;
}

void block_added(struct udev_device *device)
{
	struct block_device *block;
	L_AUTO_FREE_VAR(char *, path) = NULL;
	const char *devpath;

	devpath = udev_device_get_devpath(device);

	if (l_hashmap_lookup(block_hashmap, devpath)) {
		l_warn("New device already exists!");
		return;
	}

	block = l_new(struct block_device, 1);
	block->syspath = l_strdup(udev_device_get_syspath(device));
	block->device = l_strdup(udev_device_get_devnode(device));
	block->devpath = l_strdup(devpath);
	block->dev = udev_device_get_devnum(device);
	block->size = block_get_size(block);
	block->ignore = get_udev_bool_property(device, "EDISKSD_IGNORE");
	block->ro = sysattr_to_bool(udev_device_get_sysattr_value(device, "ro"));

	block->devlinks = str_array_from_udev_list(
			udev_device_get_devlinks_list_entry(device));

	path = block_device_get_path(block);

	if (!l_hashmap_insert(block_hashmap, devpath, block)) {
		l_warn("Failed to add block device");
		block_device_free(block);
		return;
	}

	block_register_dbus(block);

	if (is_filesystem_device(device))
		filesystem_added(block, device);
}

void block_removed(struct udev_device *device)
{
	L_AUTO_FREE_VAR(char *, path) = NULL;
	struct l_dbus *dbus = dbus_get_bus();
	struct block_device *block;
	const char *devpath;

	devpath = udev_device_get_devpath(device);

	block = l_hashmap_remove(block_hashmap, devpath);
	if (!block)
		return;

	path = block_device_get_path(block);

	l_dbus_unregister_object(dbus, path);

	filesystem_removed(block);
	block_device_free(block);
}

int block_init(void)
{
	struct l_dbus *dbus = dbus_get_bus();

	if (!l_dbus_register_interface(dbus, EDISKSD_BLOCK_INTERFACE,
				       setup_block_interface, NULL, false)) {
		l_error("Unable to register %s interface",
			EDISKSD_BLOCK_INTERFACE);
		return -EPERM;
	}

	block_hashmap = l_hashmap_string_new();
	if (!block_hashmap) {
		l_error("Failed to allocate memory");
		return -ENOMEM;
	}

	return 0;
}

void block_exit(void)
{
	struct l_dbus *dbus = dbus_get_bus();

	l_dbus_unregister_interface(dbus, EDISKSD_BLOCK_INTERFACE);
}
