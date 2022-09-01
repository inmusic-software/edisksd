// SPDX-License-Identifier: LGPL-2.1+

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <ell/ell.h>

#include "client/client.h"
#include "client/dbus.h"


struct device {
	char *dbus_path;

	char *device;

	struct l_dbus_proxy *block;
	struct l_dbus_proxy *filesystem;

	unsigned int mountpoint_seq;
	struct l_queue *mountpoints;
};

struct mountpoint {
	unsigned int seq;
	char path[];
};

static struct l_hashmap *device_hashmap;

static struct device *get_device(struct l_dbus_proxy *proxy)
{
	const char *path = l_dbus_proxy_get_path(proxy);
	struct device *dev;

	dev = l_hashmap_lookup(device_hashmap, path);
	if (dev)
		return dev;

	dev = l_new(struct device, 1);
	dev->dbus_path = l_strdup(path);
	dev->mountpoints = l_queue_new();

	l_hashmap_insert(device_hashmap, path, dev);

	return dev;
}

static void announce_mountpoint(struct device *dev, const struct mountpoint *m)
{
	l_info("filesystem mounted: %s at %s", dev->device, m->path);
}

static bool remove_unmounted(void *data, void *user_data)
{
	struct mountpoint *m = data;
	struct device *dev = user_data;

	if (m->seq == dev->mountpoint_seq)
		return false;

	l_info("filesystem unmounted: %s at %s", dev->device, m->path);
	l_free(m);
	return true;
}

static bool find_mountpoint(const void *a, const void *b)
{
	const struct mountpoint *m = a;
	const char *match = b;

	return !strcmp(m->path, match);
}

static void handle_mountpoints(struct device *dev,
			       struct l_dbus_message_iter *iter)
{
	struct l_dbus_message_iter sub_iter;

	if (!++dev->mountpoint_seq)
		dev->mountpoint_seq++; /* Avoid zero */

	while (l_dbus_message_iter_next_entry(iter, &sub_iter)) {
		const char *mountpoint;
		uint32_t n;

		if (l_dbus_message_iter_get_fixed_array(&sub_iter, &mountpoint, &n)) {
			struct mountpoint *m;

			m = l_queue_find(dev->mountpoints, find_mountpoint, mountpoint);
			if (!m) {
				size_t len = strlen(mountpoint);

				m = l_malloc(sizeof(*m) + len + 1);
				memcpy(m->path, mountpoint, len + 1);

				l_queue_push_tail(dev->mountpoints, m);
				announce_mountpoint(dev, m);
			}

			m->seq = dev->mountpoint_seq;
		}
	}

	l_queue_foreach_remove(dev->mountpoints, remove_unmounted, dev);
}

static void announce_filesystem(struct device *dev)
{
	struct l_dbus_message_iter iter;
	const char *type;

	if (l_dbus_proxy_get_property(dev->filesystem, "Type", "s", &type))
		l_info("filesystem added: %s on %s", type, dev->device);
	if (l_dbus_proxy_get_property(dev->filesystem, "Mountpoints", "aay", &iter))
		handle_mountpoints(dev, &iter);
}

static void announce_devlinks(struct l_dbus_proxy *proxy)
{
	struct l_dbus_message_iter arr, iter;

	if (!l_dbus_proxy_get_property(proxy, "DeviceLinks", "aay", &arr))
		return;

	while (l_dbus_message_iter_next_entry(&arr, &iter)) {
		const char *link;
		uint32_t n;

		if (l_dbus_message_iter_get_fixed_array(&iter, &link, &n))
			l_info("    link: %.*s", (int) n, link);
	}
}

static void proxy_added(struct l_dbus_proxy *proxy, void *user_data)
{
	const char *interface = l_dbus_proxy_get_interface(proxy);
	struct device *dev;

	dev = get_device(proxy);

	if (!strcmp(interface, EDISKSD_BLOCK_INTERFACE)) {
		const char *device;
		bool ignored;
		uint32_t n;

		dev->block = proxy;
		if (dbus_proxy_get_byte_array_property(proxy, "Device", &device, &n)) {
			if (!l_dbus_proxy_get_property(proxy, "HintIgnore", "b", &ignored))
				ignored = false;

			dev->device = l_strdup(device);
			l_info("block device added: %.*s%s", (int) n, device,
			       ignored ? " (hint:ignore)" : "");
			announce_devlinks(proxy);
		}

		if (dev->filesystem)
			announce_filesystem(dev);
	} else if (!strcmp(interface, EDISKSD_FILESYSTEM_INTERFACE)) {
		dev->filesystem = proxy;

		if (dev->block)
			announce_filesystem(dev);
	}
}

static void proxy_removed(struct l_dbus_proxy *proxy, void *user_data)
{
	const char *interface = l_dbus_proxy_get_interface(proxy);
	const char *path = l_dbus_proxy_get_path(proxy);
	struct device *dev;

	dev = l_hashmap_lookup(device_hashmap, path);
	if (!dev)
		return;

	if (!strcmp(interface, EDISKSD_BLOCK_INTERFACE)) {
		const char *device;
		uint32_t n;

		dev->block = NULL;
		if (dbus_proxy_get_byte_array_property(proxy, "Device", &device, &n))
			l_info("block device removed: %.*s", (int) n, device);
	} else if (!strcmp(interface, EDISKSD_FILESYSTEM_INTERFACE)) {
		dev->filesystem = NULL;
		l_queue_clear(dev->mountpoints, l_free);

		l_info("filesystem removed");
	}
}

static void property_changed(struct l_dbus_proxy *proxy, const char *name,
			     struct l_dbus_message *msg, void *user_data)
{
	struct device *dev = get_device(proxy);

	if (proxy == dev->filesystem && !strcmp(name, "Mountpoints")) {
		struct l_dbus_message_iter iter;

		if (l_dbus_message_get_arguments(msg, "aay", &iter))
			handle_mountpoints(dev, &iter);
	}
}

int monitor_main(struct l_dbus_client *client, int argc, char *argv[])
{
	device_hashmap = l_hashmap_string_new();

	l_dbus_client_set_proxy_handlers(client, proxy_added, proxy_removed,
					 property_changed, NULL, NULL);

	return 0;
}
