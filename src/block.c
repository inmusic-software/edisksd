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

enum connection_bus {
	CONNECTION_UNKNOWN,
	CONNECTION_USB,
	CONNECTION_MMC,
};

enum media_type {
	MEDIA_UNKNOWN,
	MEDIA_THUMB,
	MEDIA_FLASH,
	MEDIA_FLASH_CF,
	MEDIA_FLASH_MS,
	MEDIA_FLASH_SM,
	MEDIA_FLASH_SD,
	MEDIA_FLASH_SDHC,
	MEDIA_FLASH_SDXC,
	MEDIA_FLASH_SDIO,
	MEDIA_FLASH_SD_COMBO,
	MEDIA_FLASH_MMC,
};

enum media_removable_override {
	MEDIA_REMOVABLE_DEFAULT,
	MEDIA_REMOVABLE_NEVER,
	MEDIA_REMOVABLE_ALWAYS,
};

static const struct {
	const char *name;
	const char *udev_drive;
	const char *udev_media;
	enum media_removable_override removable;
} media_type_details[] = {
	[MEDIA_THUMB] = {
		.name = "thumb",
		.udev_drive = "ID_DRIVE_THUMB",
	},
	[MEDIA_FLASH] = {
		.name = "flash",
		.udev_drive = "ID_DRIVE_FLASH",
		.udev_media = "ID_DRIVE_MEDIA_FLASH",
		.removable = MEDIA_REMOVABLE_ALWAYS,
	},
	[MEDIA_FLASH_CF] = {
		.name = "flash_cf",
		.udev_drive = "ID_DRIVE_FLASH_CF",
		.udev_media = "ID_DRIVE_MEDIA_FLASH_CF",
		.removable = MEDIA_REMOVABLE_ALWAYS,
	},
	[MEDIA_FLASH_MS] = {
		.name = "flash_ms",
		.udev_drive = "ID_DRIVE_FLASH_MS",
		.udev_media = "ID_DRIVE_MEDIA_FLASH_MS",
		.removable = MEDIA_REMOVABLE_ALWAYS,
	},
	[MEDIA_FLASH_SM] = {
		.name = "flash_sm",
		.udev_drive = "ID_DRIVE_FLASH_SM",
		.udev_media = "ID_DRIVE_MEDIA_FLASH_SM",
		.removable = MEDIA_REMOVABLE_ALWAYS,
	},
	[MEDIA_FLASH_SD] = {
		.name = "flash_sd",
		.udev_drive = "ID_DRIVE_FLASH_SD",
		.udev_media = "ID_DRIVE_MEDIA_FLASH_SD",
		.removable = MEDIA_REMOVABLE_ALWAYS,
	},
	[MEDIA_FLASH_SDHC] = {
		.name = "flash_sdhc",
		.udev_drive = "ID_DRIVE_FLASH_SDHC",
		.udev_media = "ID_DRIVE_MEDIA_FLASH_SDHC",
		.removable = MEDIA_REMOVABLE_ALWAYS,
	},
	[MEDIA_FLASH_SDXC] = {
		.name = "flash_sdxc",
		.udev_drive = "ID_DRIVE_FLASH_SDXC",
		.udev_media = "ID_DRIVE_MEDIA_FLASH_SDXC",
		.removable = MEDIA_REMOVABLE_ALWAYS,
	},
	[MEDIA_FLASH_SDIO] = {
		.name = "flash_sdio",
		.udev_drive = "ID_DRIVE_FLASH_SDIO",
		.udev_media = "ID_DRIVE_MEDIA_FLASH_SDIO",
		.removable = MEDIA_REMOVABLE_ALWAYS,
	},
	[MEDIA_FLASH_SD_COMBO] = {
		.name = "flash_sd_combo",
		.udev_drive = "ID_DRIVE_FLASH_SD_COMBO",
		.removable = MEDIA_REMOVABLE_ALWAYS,
	},
	[MEDIA_FLASH_MMC] = {
		.name = "flash_mmc",
		.udev_drive = "ID_DRIVE_FLASH_MMC",
		.udev_media = "ID_DRIVE_MEDIA_FLASH_MMC",
		.removable = MEDIA_REMOVABLE_NEVER,
	},
};

struct block_device {
	char *syspath;
	char *device;
	char *devpath;

	char **devlinks;

	dev_t dev;
	uint64_t size;

	enum connection_bus connection_bus;
	enum media_type media;

	bool ignore;
	bool filesystem;
	bool removable;
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

	return sysattr_to_bool(value);
}

struct block_device *block_device_lookup_by_dev(dev_t dev)
{
	struct find_by_dev_ctx ctx = {
		.dev = dev,
	};

	l_hashmap_foreach(block_hashmap, find_by_dev, &ctx);

	return ctx.block;
}

static bool valid_path_character(char c)
{
	if (c >= 'a' && c <= 'z')
		return true;
	if (c >= 'A' && c <= 'Z')
		return true;
	if (c >= '0' && c <= '9')
		return true;
	if (c == '_' || c == '/')
		return true;

	return false;
}

char *block_device_get_path(const struct block_device *block)
{
	char *path, *c;

	path = l_strdup_printf("%s%s", EDISKSD_BASE_PATH, block->device);

	c = path;
	while (*c) {
		if (!valid_path_character(*c))
			*c = '_';
		c++;
	}

	return path;
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

static bool block_property_get_connection_bus(struct l_dbus *dbus,
					      struct l_dbus_message *message,
					      struct l_dbus_message_builder *builder,
					      void *user_data)
{
	struct block_device *block = user_data;
	const char *s;

	switch (block->connection_bus) {
	default:
		l_error("BUG: unhandled connection type: %d", block->connection_bus);
		[[fallthrough]];
	case CONNECTION_UNKNOWN:
		return false;
	case CONNECTION_USB:
		s = "usb";
		break;
	case CONNECTION_MMC:
		s = "mmc";
		break;
	}

	return l_dbus_message_builder_append_basic(builder, 's', s);
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

static bool block_property_get_media(struct l_dbus *dbus,
				     struct l_dbus_message *message,
				     struct l_dbus_message_builder *builder,
				     void *user_data)
{
	struct block_device *block = user_data;
	const char *s = media_type_details[block->media].name;

	if (!s)
		return false;

	return l_dbus_message_builder_append_basic(builder, 's', s);
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

static bool block_property_get_removable(struct l_dbus *dbus,
					 struct l_dbus_message *message,
					 struct l_dbus_message_builder *builder,
					 void *user_data)
{
	struct block_device *block = user_data;
	bool removable = block->removable;

	return l_dbus_message_builder_append_basic(builder, 'b', &removable);
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
	l_dbus_interface_property(interface, "ConnectionBus", 0, "s",
				  block_property_get_connection_bus, NULL);
	l_dbus_interface_property(interface, "Device", 0, "ay",
				  block_property_get_device, NULL);
	l_dbus_interface_property(interface, "DeviceLinks", 0, "aay",
				  block_property_get_devlinks, NULL);
	l_dbus_interface_property(interface, "DeviceNumber", 0, "t",
				  block_property_get_device_number, NULL);
	l_dbus_interface_property(interface, "HintIgnore", 0, "b",
				  block_property_get_hint_ignore, NULL);
	l_dbus_interface_property(interface, "Media", 0, "s",
				  block_property_get_media, NULL);
	l_dbus_interface_property(interface, "ReadOnly", 0, "b",
				  block_property_get_ro, NULL);
	l_dbus_interface_property(interface, "Removable", 0, "b",
				  block_property_get_removable, NULL);
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

static bool str_array_udev_list_changed(char **array, struct udev_list_entry *list)
{
	struct udev_list_entry *e;
	size_t n = 0;

	udev_list_entry_foreach(e, list) {
		if (!array[n])
			return true;

		if (strcmp(array[n], udev_list_entry_get_name(e)))
			return true;

		n++;
	}

	if (array[n])
		return true;

	return false;
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

static enum connection_bus block_get_connection_bus(struct udev_device *device)
{
	struct udev_device *parent;

	if (udev_device_get_parent_with_subsystem_devtype(device, "usb", "usb_interface"))
		return CONNECTION_USB;

	if (l_str_has_prefix(udev_device_get_sysname(device), "mmcblk"))
		return CONNECTION_MMC;

	return CONNECTION_UNKNOWN;
}

static enum media_type block_get_mmc_media(struct udev_device *device)
{
	const char *value;

	value = udev_device_get_sysattr_value(device, "type");
	if (!value)
		return MEDIA_UNKNOWN;

	if (!strcmp(value, "MMC"))
		return MEDIA_FLASH_MMC;
	if (!strcmp(value, "SD"))
		return MEDIA_FLASH_SD;
	if (!strcmp(value, "SDIO"))
		return MEDIA_FLASH_SDIO;

	return MEDIA_UNKNOWN;
}

static enum media_type block_get_media(struct udev_device *device)
{
	struct udev_device *mmc;

	/* First pass - look for specific media set by udev rules. */
	for (size_t i = 0; i < L_ARRAY_SIZE(media_type_details); i++)
		if (get_udev_bool_property(device, media_type_details[i].udev_media))
			return (enum media_type) i;

	/* If nothing succeeds there, use the first type supported by the drive. */
	for (size_t i = 0; i < L_ARRAY_SIZE(media_type_details); i++)
		if (get_udev_bool_property(device, media_type_details[i].udev_drive))
			return (enum media_type) i;

	/* For MMC we can inspect the parent's type to refine the media type. */
	mmc = udev_device_get_parent_with_subsystem_devtype(device, "mmc", NULL);
	if (mmc)
		return block_get_mmc_media(mmc);

	return MEDIA_UNKNOWN;
}

static bool block_is_removable(struct udev_device *device, enum media_type media,
			       enum connection_bus bus)
{
	struct block_device *parent;
	const char *value;

	/* Is there an override for this specific device? */
	value = udev_device_get_property_value(device, "EDISKSD_REMOVABLE");
	l_info("%s for %s: EDISKS_REMOVABLE=%s media=%d", __func__,
			udev_device_get_devpath(device),
			value ? value : "???", media);

	if (value)
		return sysattr_to_bool(value);

	/* Do we have special handling for this media type? */
	switch (media_type_details[media].removable) {
	case MEDIA_REMOVABLE_DEFAULT:
		break;

	case MEDIA_REMOVABLE_NEVER:
		return false;

	case MEDIA_REMOVABLE_ALWAYS:
		return true;
	}

	/* Some bus types are always removable. */
	switch (bus) {
	case CONNECTION_USB:
		return true;

	default:
		break;
	}

	/*
	 * Use the kernel's "removable" attribute - we may have to search for
	 * the "drive" block device as partition devices do not have this
	 * attribute.
	 */
	value = udev_device_get_sysattr_value(device, "removable");
	if (value)
		return sysattr_to_bool(value);

	device = udev_device_get_parent(device);
	if (strcmp(udev_device_get_subsystem(device), "block"))
		return false;

	parent = block_device_lookup_by_dev(udev_device_get_devnum(device));
	if (!parent)
		return false;

	return parent->removable;
}

void block_added(struct udev_device *device)
{
	struct block_device *block;
	L_AUTO_FREE_VAR(char *, path) = NULL;
	const char *devpath;

	devpath = udev_device_get_devpath(device);

	if (l_hashmap_lookup(block_hashmap, devpath)) {
		l_warn("New device already exists!");
		block_changed(device);
		return;
	}

	block = l_new(struct block_device, 1);
	block->syspath = l_strdup(udev_device_get_syspath(device));
	block->device = l_strdup(udev_device_get_devnode(device));
	block->devpath = l_strdup(devpath);
	block->dev = udev_device_get_devnum(device);
	block->size = block_get_size(block);
	block->connection_bus = block_get_connection_bus(device);
	block->media = block_get_media(device);
	block->ignore = get_udev_bool_property(device, "EDISKSD_IGNORE");
	block->filesystem = is_filesystem_device(device);
	block->removable = block_is_removable(device, block->media, block->connection_bus);
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

	if (block->filesystem)
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

void block_changed(struct udev_device *device)
{
	L_AUTO_FREE_VAR(char *, path) = NULL;
	struct l_dbus *dbus = dbus_get_bus();
	enum connection_bus connection_bus;
	struct udev_list_entry *devlinks;
	struct block_device *block;
	enum media_type media;
	const char *devpath;
	bool removable;

	devpath = udev_device_get_devpath(device);

	block = l_hashmap_lookup(block_hashmap, devpath);
	if (!block)
		return;

	path = block_device_get_path(block);

	if (block->size != block_get_size(block)) {
		block->size = block_get_size(block);
		l_dbus_property_changed(dbus, path,
					EDISKSD_BLOCK_INTERFACE,
					"Size");
	}

	if (block->ignore != get_udev_bool_property(device, "EDISKSD_IGNORE")) {
		block->ignore = get_udev_bool_property(device, "EDISKSD_IGNORE");
		l_dbus_property_changed(dbus, path,
					EDISKSD_BLOCK_INTERFACE,
					"HintIgnore");
	}

	if (block->ro != sysattr_to_bool(udev_device_get_sysattr_value(device, "ro"))) {
		block->ro = sysattr_to_bool(udev_device_get_sysattr_value(device, "ro"));
		l_dbus_property_changed(dbus, path,
					EDISKSD_BLOCK_INTERFACE,
					"ReadOnly");
	}

	connection_bus = block_get_connection_bus(device);
	if (block->connection_bus != connection_bus) {
		block->connection_bus = connection_bus;
		l_dbus_property_changed(dbus, path,
					EDISKSD_BLOCK_INTERFACE,
					"ConnectionBus");
	}

	media = block_get_media(device);
	if (block->media != media) {
		block->media = media;
		l_dbus_property_changed(dbus, path,
					EDISKSD_BLOCK_INTERFACE,
					"Media");
	}

	removable = block_is_removable(device, block->media, block->connection_bus);
	if (block->removable != removable) {
		block->removable = removable;
		l_dbus_property_changed(dbus, path,
					EDISKSD_BLOCK_INTERFACE,
					"Removable");
	}

	devlinks = udev_device_get_devlinks_list_entry(device);
	if (str_array_udev_list_changed(block->devlinks, devlinks)) {
		l_strv_free(block->devlinks);
		block->devlinks = str_array_from_udev_list(devlinks);
		l_dbus_property_changed(dbus, path,
					EDISKSD_BLOCK_INTERFACE,
					"DeviceLinks");
	}

	if (is_filesystem_device(device)) {
		if (block->filesystem)
			filesystem_changed(block, device);
		else
			filesystem_added(block, device);

		block->filesystem = true;
	} else {
		filesystem_removed(block);
		block->filesystem = false;
	}
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
