// SPDX-License-Identifier: LGPL-2.1+

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <getopt.h>
#include <signal.h>
#include <stdio.h>

#include <ell/ell.h>

#include "client/client.h"
#include "client/dbus.h"


static const struct {
	const char *cmd;
	int (*fn)(struct l_dbus_client *, int, char **);
} commands[] = {
	{ "monitor",	monitor_main, },
	{ "mount",	mount_main, },
	{ "umount",	unmount_main, },
	{ "unmount",	unmount_main, },
};

static void signal_handler(uint32_t signo, void *user_data)
{
	switch (signo) {
	case SIGINT:
	case SIGTERM:
		l_info("Exiting due to signal");
		l_main_quit();
		break;
	}
}

static bool is_umount_helper(const char *progname)
{
	return !!strstr(progname, "umount.edisksd");
}

static int run_command(struct l_dbus_client *client, int argc, char *argv[])
{
	const char *command;
	size_t i;

	if (is_umount_helper(argv[0])) {
		command = "unmount";
	} else {
		command = argv[1];

		argc--;
		argv++;
	}

	for (i = 0; i < L_ARRAY_SIZE(commands); i++)
		if (!strcmp(commands[i].cmd, command))
			return commands[i].fn(client, argc, argv);

	l_error("Unknown command: %s", argv[0]);
	return EXIT_FAILURE;
}

int main(int argc, char *argv[])
{
	struct l_dbus_client *client;
	struct l_dbus *dbus;
	int ret;

	if (!l_main_init())
		return EXIT_FAILURE;

	l_log_set_stderr();

	if (argc < 2)
		return EXIT_FAILURE;

	dbus = l_dbus_new_default(L_DBUS_SYSTEM_BUS);
	client = l_dbus_client_new(dbus, EDISKSD_SERVICE, EDISKSD_BASE_PATH);

	ret = run_command(client, argc, argv);
	if (ret)
		goto failed_run;

	ret = l_main_run_with_signal(signal_handler, NULL);

failed_run:
	l_dbus_client_destroy(client);
	l_dbus_destroy(dbus);
	l_main_exit();
	return ret;
}
