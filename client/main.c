// SPDX-License-Identifier: LGPL-2.1+

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <err.h>
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

struct context {
	struct l_dbus *dbus;
	struct l_dbus_client *client;
	int argc;
	char **argv;

	int version_attempts;
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

static void run_command(struct context *ctx)
{
	const char *command;
	size_t i;

	if (is_umount_helper(ctx->argv[0])) {
		command = "unmount";
	} else {
		command = ctx->argv[1];

		ctx->argc--;
		ctx->argv++;
	}

	ctx->client = l_dbus_client_new(ctx->dbus, EDISKSD_SERVICE, EDISKSD_BASE_PATH);

	for (i = 0; i < L_ARRAY_SIZE(commands); i++) {
		if (!strcmp(commands[i].cmd, command)) {
			int ret;

			ret = commands[i].fn(ctx->client, ctx->argc, ctx->argv);
			if (ret)
				exit(ret);

			return;
		}
	}

	l_error("Unknown command: %s", ctx->argv[0]);
	exit(EXIT_FAILURE);
}

static void check_daemon_version(struct context *ctx);

static void daemon_version_reply(struct l_dbus_message *message, void *user_data)
{
	struct context *ctx = user_data;
	struct l_dbus_message_iter value;
	const char *version;

	if (l_dbus_message_is_error(message)) {
		const char *name, *text;
		if (l_dbus_message_get_error(message, &name, &text) &&
		    !strcmp(name, "org.freedesktop.DBus.Error.NotFound") &&
		    ++ctx->version_attempts < 3) {
			check_daemon_version(ctx);
			return;
		}

		l_error("failed to get daemon version: %s: %s", name, text);
		exit(1);
	}

	if (!l_dbus_message_get_arguments(message, "v", &value) ||
	    !l_dbus_message_iter_get_variant(&value, "s", &version))
		errx(1, "did not recognise daemon response");

	l_info("Daemon version: %s", version);
	run_command(ctx);
}

static void check_daemon_version(struct context *ctx)
{
	struct l_dbus_message *message;

	message = l_dbus_message_new_method_call(ctx->dbus, EDISKSD_SERVICE,
						 EDISKSD_BASE_PATH,
						 L_DBUS_INTERFACE_PROPERTIES,
						 "Get");
	l_dbus_message_set_arguments(message, "ss",
				     EDISKSD_DAEMON_INTERFACE, "Version");

	l_dbus_send_with_reply(ctx->dbus, message, daemon_version_reply, ctx, NULL);
}

int main(int argc, char *argv[])
{
	struct context ctx = {
		.argc = argc,
		.argv = argv,
	};
	struct l_dbus *dbus;
	int ret;

	if (!l_main_init())
		return EXIT_FAILURE;

	l_log_set_stderr();

	if (argc < 2)
		return EXIT_FAILURE;

	ctx.dbus = l_dbus_new_default(L_DBUS_SYSTEM_BUS);

	/*
	 * Ensure we start with a method call which will activate the service
	 * if it is not already running.
	 */
	check_daemon_version(&ctx);

	ret = l_main_run_with_signal(signal_handler, NULL);

failed_run:
	l_dbus_client_destroy(ctx.client);
	l_dbus_destroy(ctx.dbus);
	l_main_exit();
	return ret;
}
