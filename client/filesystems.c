// SPDX-License-Identifier: LGPL-2.1+

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <ell/ell.h>

#include "client/client.h"
#include "client/dbus.h"


struct device {
	struct l_dbus_proxy *block;
	struct l_dbus_proxy *filesystem;
};

static struct l_queue *devices;
static filesystem_cmd_t command;

static struct device *new_device(void)
{
	struct device *device = l_new(struct device, 1);

	l_queue_push_tail(devices, device);
	return device;
}

static void free_device(struct device *device)
{
	l_queue_remove(devices, device);
	l_free(device);
}

static bool match_block_path(const void *a, const void *b)
{
	const struct device *device = a;
	const char *path = b;

	if (!device->block)
		return false;

	return !strcmp(l_dbus_proxy_get_path(device->block), path);
}

static bool match_filesystem_path(const void *a, const void *b)
{
	const struct device *device = a;
	const char *path = b;

	if (!device->filesystem)
		return false;

	return !strcmp(l_dbus_proxy_get_path(device->filesystem), path);
}

static void proxy_added(struct l_dbus_proxy *proxy, void *user_data)
{
	const char *interface = l_dbus_proxy_get_interface(proxy);
	const char *path = l_dbus_proxy_get_path(proxy);
	struct device *device;

	if (!strcmp(interface, EDISKSD_BLOCK_INTERFACE)) {
		device = l_queue_find(devices, match_filesystem_path, path);
		if (!device)
			device = new_device();

		device->block = proxy;
	} else if (!strcmp(interface, EDISKSD_FILESYSTEM_INTERFACE)) {
		device = l_queue_find(devices, match_block_path, path);
		if (!device)
			device = new_device();

		device->filesystem = proxy;
	}
}

static void proxy_removed(struct l_dbus_proxy *proxy, void *user_data)
{
	const char *interface = l_dbus_proxy_get_interface(proxy);
	const char *path = l_dbus_proxy_get_path(proxy);
	struct device *device;

	if (!strcmp(interface, EDISKSD_BLOCK_INTERFACE)) {
		device = l_queue_find(devices, match_block_path, path);
		if (!device)
			return;

		device->block = NULL;
		if (!device->filesystem)
			free_device(device);
	} else if (!strcmp(interface, EDISKSD_FILESYSTEM_INTERFACE)) {
		device = l_queue_find(devices, match_filesystem_path, path);
		if (!device)
			return;

		device->filesystem = NULL;
		if (!device->block)
			free_device(device);
	}
}

static void client_ready(struct l_dbus_client *client, void *user_data)
{
	command();
}

void set_filesystem_cmd(struct l_dbus_client *client, filesystem_cmd_t cmd)
{
	devices = l_queue_new();
	command = cmd;

	l_dbus_client_set_proxy_handlers(client, proxy_added, proxy_removed,
					 NULL, NULL, NULL);
	l_dbus_client_set_ready_handler(client, client_ready, NULL, NULL);
}

static bool match_device(const void *a, const void *b)
{
	const struct device *device = a;
	const char *path = b;
	const char *property_path;
	uint32_t n;

	if (!device->block || !device->filesystem)
		return false;

	if (!dbus_proxy_get_byte_array_property(device->block, "Device",
						&property_path, &n))
		return false;

	return !strcmp(property_path, path);
}

struct l_dbus_proxy *find_filesystem_for_device(const char *devpath)
{
	struct device *device = l_queue_find(devices, match_device, devpath);

	if (!device)
		return NULL;

	return device->filesystem;
}

static bool match_mountpoint(const void *a, const void *b)
{
	const struct device *device = a;
	const char *mountpoint = b;
	struct l_dbus_message_iter array;
	struct l_dbus_message_iter bytes;

	if (!device->block || !device->filesystem)
		return false;

	if (!l_dbus_proxy_get_property(device->filesystem, "Mountpoints",
				       "aay", &array))
		return false;

	while (l_dbus_message_iter_next_entry(&array, &bytes)) {
		const char *value;
		uint32_t n;

		if (!l_dbus_message_iter_get_fixed_array(&bytes, &value, &n))
			continue;

		if (!strcmp(value, mountpoint))
			return true;
	}

	return false;
}

struct l_dbus_proxy *find_filesystem_for_mountpoint(const char *mountpoint)
{
	struct device *device = l_queue_find(devices, match_mountpoint, mountpoint);

	if (!device)
		return NULL;

	return device->filesystem;
}
