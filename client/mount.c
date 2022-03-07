// SPDX-License-Identifier: LGPL-2.1+

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <ell/ell.h>

#include "client/client.h"
#include "client/dbus.h"


static char *device;

static void mount_setup(struct l_dbus_message *message, void *user_data)
{
	l_dbus_message_set_arguments(message, "a{sv}", 0);
}

static void mount_reply(struct l_dbus_proxy *proxy,
			struct l_dbus_message *result,
			void *user_data)
{
	const char *path;

	if (l_dbus_message_is_error(result)) {
		const char *name;
		const char *text;

		if (!l_dbus_message_get_error(result, &name, &text))
			l_error("failed to mount");
		else
			l_error("failed to mount: %s", text);

		exit(1);
	}

	if (!l_dbus_message_get_arguments(result, "s", &path)) {
		l_error("failed to get mount path");
		exit(1);
	}

	l_info("%s mounted at %s", device, path);
	l_main_quit();
}

static void mount_cmd(void)
{
	struct l_dbus_proxy *proxy = find_filesystem_for_device(device);
	uint32_t call_id;

	if (!proxy) {
		l_error("no such device: %s", device);
		exit(1);
	}

	call_id = l_dbus_proxy_method_call(proxy, "Mount",
					   mount_setup, mount_reply,
					   NULL, NULL);

	if (!call_id) {
		l_error("failed to call mount method");
		exit(1);
	}
}

int mount_main(struct l_dbus_client *client, int argc, char *argv[])
{
	if (argc != 2) {
		l_error("usage: edisksctl mount <device>");
		return 1;
	}

	device = l_strdup(argv[1]);
	set_filesystem_cmd(client, mount_cmd);

	return 0;
}
