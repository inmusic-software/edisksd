// SPDX-License-Identifier: LGPL-2.1+

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <errno.h>

#include <ell/ell.h>
#include <libudev.h>

#include "src/block.h"
#include "src/udev.h"

static struct udev_monitor *monitor;
static struct l_io *monitor_io;

static void dispatch_device(struct udev_device *device)
{
	const char *action = udev_device_get_action(device);

	if (strcmp(udev_device_get_subsystem(device), "block"))
		return;

	l_info("%s device: %s", action ? action : "initial",
		udev_device_get_devpath(device));

	/*
	 * action will be null for the initial scan, this is equivalent to
	 * "add" as far as we're concerned.
	 */
	if (!action || !strcmp(action, "add"))
		block_added(device);
	else if (!strcmp(action, "remove"))
		block_removed(device);
	else if (!strcmp(action, "change"))
		block_changed(device);
}

static bool udev_read_callback(struct l_io *io, void *user_data)
{
	for (;;) {
		struct udev_device *device;

		device = udev_monitor_receive_device(monitor);
		if (!device)
			break;

		dispatch_device(device);
		udev_device_unref(device);
	}

	return true;
}

static int udev_scan_initial_devices(struct udev *udev)
{
	struct udev_enumerate *enumerate;
	struct udev_list_entry *entry;
	struct udev_list_entry *list;
	int ret = -ENODEV;

	enumerate = udev_enumerate_new(udev);
	if (!enumerate) {
		l_error("Failed to set up udev initial enumeration");
		return -ENOMEM;
	}

	if (udev_enumerate_add_match_subsystem(enumerate, "block") < 0)
		l_warn("Failed to add udev enumeration filter");

	if (udev_enumerate_scan_devices(enumerate) < 0) {
		l_error("Failed to scan initial devices");
		goto failed_enumerate_scan;
	}

	list = udev_enumerate_get_list_entry(enumerate);
	if (!list) {
		l_error("Failed to get initial devices");
		goto failed_enumerate_scan;
	}

	udev_list_entry_foreach (entry, list) {
		struct udev_device *device;
		const char *syspath;

		syspath = udev_list_entry_get_name(entry);
		device = udev_device_new_from_syspath(udev, syspath);
		if (!device) {
			l_warn("Failed to look up device %s", syspath);
			continue;
		}

		dispatch_device(device);
		udev_device_unref(device);
	}

	ret = 0;

failed_enumerate_scan:
	udev_enumerate_unref(enumerate);

	return ret;
}

int udev_init(void)
{
	struct udev *udev;

	udev = udev_new();
	if (!udev) {
		l_error("Failed to initialize udev");
		return -1;
	}

	monitor = udev_monitor_new_from_netlink(udev, "udev");
	if (!monitor) {
		l_error("Failed to initialize udev monitor");
		goto failed_udev_monitor;
	}

	monitor_io = l_io_new(udev_monitor_get_fd(monitor));
	if (!monitor_io) {
		l_error("Failed to set up udev monitor");
		goto failed_io;
	}

	if (!l_io_set_read_handler(monitor_io, udev_read_callback, NULL, NULL)) {
		l_error("Failed to set udev ready handler");
		goto failed_io_handler;
	}

	if (udev_monitor_filter_add_match_subsystem_devtype(monitor, "block", NULL) < 0)
		l_warn("Failed to add udev filter");

	if (udev_monitor_enable_receiving(monitor) < 0) {
		l_error("Failed to enable udev monitor");
		goto failed_enable;
	}

	if (udev_scan_initial_devices(udev) < 0)
		goto failed_enable;

	return 0;

failed_enable:
failed_io_handler:
	l_io_destroy(monitor_io);
failed_io:
	udev_monitor_unref(monitor);
failed_udev_monitor:
	udev_unref(udev);
	return -1;
}

void udev_exit(void)
{
	struct udev *udev;

	if (!monitor)
		return;

	l_io_destroy(monitor_io);

	udev = udev_monitor_get_udev(monitor);
	udev_monitor_unref(monitor);
	udev_unref(udev);
}
