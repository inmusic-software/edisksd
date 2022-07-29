// SPDX-License-Identifier: LGPL-2.1+

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <getopt.h>
#include <stdio.h>

#include <ell/ell.h>

#include "client/client.h"
#include "client/dbus.h"


struct mount_opt {
	const char *key;
	const char *value;
};

static char *device;
static char *fstype;
static struct l_queue *mount_opts;


static void mount_append_opt(struct l_dbus_message_builder *builder,
			     const char *key, const char *value)
{
	l_dbus_message_builder_enter_dict(builder, "sv");
	l_dbus_message_builder_append_basic(builder, 's', key);

	l_dbus_message_builder_enter_variant(builder, "s");
	l_dbus_message_builder_append_basic(builder, 's', value);
	l_dbus_message_builder_leave_variant(builder);

	l_dbus_message_builder_leave_dict(builder);
}

static void mount_append_queued_opt(void *data, void *user_data)
{
	const struct mount_opt *opt = data;
	struct l_dbus_message_builder *builder = user_data;

	mount_append_opt(builder, opt->key, opt->value ? opt->value : "");
}

static void mount_setup(struct l_dbus_message *message, void *user_data)
{
	struct l_dbus_message_builder *builder;

	builder = l_dbus_message_builder_new(message);
	l_dbus_message_builder_enter_array(builder, "{sv}");

	if (fstype)
		mount_append_opt(builder, "type", fstype);

	l_queue_foreach(mount_opts, mount_append_queued_opt, builder);

	l_dbus_message_builder_leave_array(builder);

	l_dbus_message_builder_finalize(builder);
	l_dbus_message_builder_destroy(builder);
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

static void mount_handle_options_arg(const char *arg)
{
	char **split = l_strsplit(arg, ',');
	char **item;

	for (item = split; *item; item++) {
		char *value = strchr(*item, '=');
		struct mount_opt *opt;

		if (value)
			*value++ = '\0';

		opt = l_new(struct mount_opt, 1);
		opt->key = l_strdup(*item);
		opt->value = l_strdup(value);

		l_queue_push_tail(mount_opts, opt);
	}

	l_strv_free(split);
}

static void mount_usage(FILE *f)
{
	fprintf(f, "usage: edisksctl mount [options] <device>\n\n");
	fprintf(f, "Options are:\n");
	fprintf(f, "  -t, --type <fstype>\n");
	fprintf(f, "  -o, --options <opts>\n");
}

int mount_main(struct l_dbus_client *client, int argc, char *argv[])
{
	mount_opts = l_queue_new();

	for (;;) {
		static struct option opts[] = {
			{ "help",	required_argument,	0, 'h', },
			{ "options",	required_argument,	0, 'o', },
			{ "type",	required_argument,	0, 't', },
			{ NULL }
		};
		int c;

		c = getopt_long(argc, argv, "t:o:h", opts, NULL);
		if (c == -1)
			break;

		switch (c) {
		case 'h':
			mount_usage(stdout);
			return 0;

		case 'o':
			mount_handle_options_arg(optarg);
			break;

		case 't':
			l_free(fstype);
			fstype = l_strdup(optarg);
			break;

		default:
			return 1;
		}
	}

	if (argc - optind != 1) {
		mount_usage(stderr);
		return 1;
	}

	device = l_strdup(argv[optind]);
	set_filesystem_cmd(client, mount_cmd);

	return 0;
}
