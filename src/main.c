// SPDX-License-Identifier: LGPL-2.1+

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <getopt.h>
#include <signal.h>
#include <stdio.h>

#include <ell/ell.h>

#include "src/edisksd.h"

#include "src/block.h"
#include "src/dbus.h"
#include "src/filesystem.h"
#include "src/mount.h"
#include "src/task.h"
#include "src/udev.h"

static struct l_dbus *dbus;
static struct l_settings *edisksd_config;

static void signal_handler(uint32_t signo, void *user_data)
{
	switch (signo) {
	case SIGINT:
	case SIGTERM:
		l_info("Terminate");
		l_main_quit();
		break;
	}
}

static bool edisks_property_get_version(struct l_dbus *dbus,
					struct l_dbus_message *message,
					struct l_dbus_message_builder *builder,
					void *user_data)
{
	l_dbus_message_builder_append_basic(builder, 's', VERSION);
	return true;
}

static void request_name_callback(struct l_dbus *dbus, bool success,
				  bool queued, void *user_data)
{
	if (!success) {
		l_error("Name request failed");
		goto fail_exit;
	}

	if (!l_dbus_object_manager_enable(dbus, "/"))
		l_warn("Unable to register the ObjectManager");

	if (!l_dbus_object_add_interface(dbus, EDISKSD_BASE_PATH,
					 EDISKSD_DAEMON_INTERFACE,
					 NULL) ||
	    !l_dbus_object_add_interface(dbus, EDISKSD_BASE_PATH,
					 L_DBUS_INTERFACE_PROPERTIES,
					 NULL))
		l_warn("Unable to add %s and/or %s at %s",
			EDISKSD_DAEMON_INTERFACE, L_DBUS_INTERFACE_PROPERTIES,
			EDISKSD_BASE_PATH);

	return;

fail_exit:
	l_main_quit();
}

static void edisksd_setup_daemon_interface(struct l_dbus_interface *interface)
{
	l_dbus_interface_property(interface, "Version", 0, "s",
				  edisks_property_get_version, NULL);
}

static void dbus_ready(void *user_data)
{
	struct l_dbus *dbus = user_data;

	l_dbus_name_acquire(dbus, EDISKSD_SERVICE, false, false, false,
			    request_name_callback, NULL);

	l_dbus_register_interface(dbus, EDISKSD_DAEMON_INTERFACE,
				  edisksd_setup_daemon_interface,
				  NULL, false);
}

static void dbus_disconnected(void *user_data)
{
	l_info("D-Bus disconnected, quitting...");
	l_main_quit();
}

struct l_dbus *dbus_get_bus(void)
{
	return dbus;
}

int main(int argc, char *argv[])
{
	int exit_status = EXIT_FAILURE;
	const char *config_dir;
	char **config_dirs;
	int i;

	if (argc - optind > 0) {
		fprintf(stderr, "Invalid command line parameters\n");
		return EXIT_FAILURE;
	}

	l_log_set_stderr();

	if (!l_main_init())
		return EXIT_FAILURE;

	config_dir = DAEMON_CONFIGDIR;
	edisksd_config = l_settings_new();
	config_dirs = l_strsplit(config_dir, ':');

	for (i = 0; config_dirs[i]; i++) {
		L_AUTO_FREE_VAR(char *, path) =
			l_strdup_printf("%s/%s", config_dirs[i], "main.conf");

		if (!l_settings_load_from_file(edisksd_config, path))
			continue;

		l_info("Loaded configuration from %s", path);
		break;
	}

	l_strv_free(config_dirs);

	dbus = l_dbus_new_default(L_DBUS_SYSTEM_BUS);
	if (!dbus) {
		l_error("Failed to initialize D-Bus");
		goto failed_dbus;
	}

	l_dbus_set_ready_handler(dbus, dbus_ready, dbus, NULL);
	l_dbus_set_disconnect_handler(dbus, dbus_disconnected, NULL, NULL);

	if (block_init() < 0)
		goto failed_block;

	if (filesystem_init() < 0)
		goto failed_filesystem;

	if (udev_init() < 0)
		goto failed_udev;

	if (mount_init() < 0)
		goto failed_mount;

	if (task_init() < 0)
		goto failed_task;

	exit_status = l_main_run_with_signal(signal_handler, NULL);

	// Task must shutdown first so that in-progress tasks are completed.
	task_exit();
failed_task:
	mount_exit();
failed_mount:
	udev_exit();
failed_udev:
	filesystem_exit();
failed_filesystem:
	block_exit();
failed_block:
	l_dbus_destroy(dbus);
	dbus = NULL;
failed_dbus:
	l_settings_free(edisksd_config);
	l_main_exit();

	return exit_status;
}
