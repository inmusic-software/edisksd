// SPDX-License-Identifier: LGPL-2.1+

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <errno.h>
#include <limits.h>
#include <unistd.h>

#include <sys/stat.h>

#include <ell/ell.h>
#include <libmount.h>
#include <libudev.h>

#include "src/edisksd.h"

#include "src/block.h"
#include "src/dbus.h"
#include "src/filesystem.h"
#include "src/task.h"

struct filesystem {
	struct block_device *block;

	char *type;
	char *devnode;
	dev_t dev;

	char *label;
	char *uuid;

	struct l_queue *mountpoints;

	/* Mountpoint memory is owned by managed_mountpoints. */
	const char *pending_mountpoint;
	struct l_dbus_message *mount_pending;
	char *mount_type;
	char *mount_opts;

	int mount_result;
};
static struct l_queue *filesystems;
static struct l_queue *managed_mountpoints;

static bool filesystem_match_dev(const void *a, const void *b)
{
	const struct filesystem *fs = a;
	const dev_t *dev = b;

	return fs->dev == *dev;
}

static struct filesystem *filesystem_lookup_by_dev(dev_t dev)
{
	return l_queue_find(filesystems, filesystem_match_dev, &dev);
}

static void append_escaped(struct l_string *str, const char *s)
{
	while (*s) {
		char *end = strchrnul(s, '/');

		l_string_append_fixed(str, s, end - s);

		if (*end) {
			l_string_append_c(str, '_');
			end++;
		}

		s = end;
	}
}

static char *create_mountpoint(struct filesystem *fs)
{
	unsigned int suffix;

	for (suffix = 0; suffix < UINT_MAX; suffix++) {
		struct l_string *str = l_string_new(0);
		char *mountpoint;

		l_string_append(str, "/media/");

		if (fs->label && fs->label[0])
			append_escaped(str, fs->label);
		else if (fs->uuid && fs->uuid[0])
			append_escaped(str, fs->uuid);
		else
			l_string_append(str, "disk");

		if (suffix)
			l_string_append_printf(str, "%u", suffix);

		mountpoint = l_string_unwrap(str);
		if (!mkdir(mountpoint, 0777))
			return mountpoint;

		l_free(mountpoint);
	}

	return NULL;
}

static void mount_run(void *data)
{
	struct filesystem *fs = data;
	struct libmnt_context *ctx;
	char buf[4096] = { 0 };
	int ret = -ENOMEM;

	ctx = mnt_new_context();
	if (!ctx)
		goto failed;

	ret = mnt_context_set_source(ctx, fs->devnode);
	if (ret)
		goto failed_context;
	ret = mnt_context_set_target(ctx, fs->pending_mountpoint);
	if (ret)
		goto failed_context;
	ret = mnt_context_set_fstype(ctx, fs->mount_type);
	if (ret)
		goto failed_context;
	ret = mnt_context_set_options(ctx, fs->mount_opts);
	if (ret)
		goto failed_context;

	ret = mnt_context_mount(ctx);
	ret = mnt_context_get_excode(ctx, ret, buf, sizeof(buf));

failed_context:
	mnt_free_context(ctx);
failed:
	fs->mount_result = ret;
}

static void mount_done(void *data)
{
	struct filesystem *fs = data;
	struct l_dbus_message *reply;

	if (fs->mount_result) {
		reply = dbus_error_failed(fs->mount_pending);

		if (rmdir(fs->pending_mountpoint) < 0)
			l_warn("failed to delete mountpoint");

		l_queue_remove(managed_mountpoints,
			       (char *) fs->pending_mountpoint);
		l_free((char *) fs->pending_mountpoint);
	} else {
		reply = l_dbus_message_new_method_return(fs->mount_pending);
		l_dbus_message_set_arguments(reply, "s", fs->pending_mountpoint);
	}

	l_dbus_send(dbus_get_bus(), reply);
	l_dbus_message_unref(fs->mount_pending);

	fs->pending_mountpoint = NULL;
	fs->mount_pending = NULL;
}

static bool mount_opt_is_permitted(const char *filesystem, const char *key)
{
	static const char *const all_permitted[] = {
		"async",
		"atime",
		"noatime",
		/* NOTE: dev is not permitted */
		"nodev",
		"diratime",
		"nodiratime",
		"dirsync",
		"exec",
		"noexec",
		"relatime",
		"norelatime",
		"strictatime",
		"nostrictatime",
		"lazytime",
		/* NOTE: suid is not permitted */
		"nosuid",
		"ro",
		"rw",
		"sync",
		"nosymfollow",
		"noload",

		/* TODO: move these to filesystem-specific lists/config */
		"flush",
		"showexec",
		"usefree",
	};

	size_t i;

	for (i = 0; i < L_ARRAY_SIZE(all_permitted); i++)
		if (!strcmp(key, all_permitted[i]))
			return true;

	return false;
}

static struct l_dbus_message *filesystem_mount(struct l_dbus *dbus,
					       struct l_dbus_message *message,
					       void *user_data)
{
	struct filesystem *fs = user_data;
	struct l_dbus_message_iter options;
	struct l_dbus_message_iter variant;
	struct l_string *optstr;
	const char *key;
	char *mountpoint;
	int ret;

	if (!l_dbus_message_get_arguments(message, "a{sv}", &options))
		return dbus_error_invalid_args(message);

	if (!l_queue_isempty(fs->mountpoints))
		return dbus_error_already_mounted(message);
	if (fs->mount_pending)
		return dbus_error_busy(message);

	optstr = l_string_new(0);
	l_string_append(optstr, "uhelper=edisksd,nodev,nosuid");

	l_free(fs->mount_type);
	fs->mount_type = l_strdup(fs->type);

	while (l_dbus_message_iter_next_entry(&options, &key, &variant)) {
		const char *value;

		/*
		 * All values are expected to be strings (for now) so extract
		 * the value early.
		 */
		if (!l_dbus_message_iter_next_entry(&variant, &value))
			goto invalid;

		/* Block smuggled options. */
		if (strchr(key, ',') || strchr(value, ','))
			goto invalid;

		if (!strcmp(key, "type")) {
			l_free(fs->mount_type);
			fs->mount_type = l_strdup(value);
			continue;
		}

		if (!mount_opt_is_permitted(fs->mount_type, key))
			goto invalid;

		if (!*value)
			l_string_append_printf(optstr, ",%s", key);
		else
			l_string_append_printf(optstr, ",%s=%s", key, value);
	}

	l_free(fs->mount_opts);
	fs->mount_opts = l_string_unwrap(optstr);

	mountpoint = create_mountpoint(fs);
	if (!mountpoint)
		return dbus_error_failed(message);

	fs->pending_mountpoint = mountpoint;
	fs->mount_pending = l_dbus_message_ref(message);
	ret = task_start(mount_run, mount_done, fs);
	if (ret < 0) {
		l_free(mountpoint);
		l_dbus_message_unref(message);
		fs->pending_mountpoint = NULL;
		fs->mount_pending = NULL;
		return dbus_error_from_errno(-ret, message);
	}

	l_queue_push_tail(managed_mountpoints, mountpoint);
	return NULL;

invalid:
	l_string_free(optstr);
	return dbus_error_invalid_args(message);
}

struct unmount_ctx {
	struct l_dbus_message *pending;
	const char *devnode;
	const char *mountpoint;
	int result;
};

static void unmount_run(void *data)
{
	struct unmount_ctx *context = data;
	struct libmnt_context *ctx;
	char buf[4096] = { 0 };
	int ret = -ENOMEM;

	ctx = mnt_new_context();
	if (!ctx)
		goto failed;

	ret = mnt_context_set_source(ctx, context->devnode);
	if (ret)
		goto failed_context;
	ret = mnt_context_set_target(ctx, context->mountpoint);
	if (ret)
		goto failed_context;
	ret = mnt_context_disable_helpers(ctx, 1);
	if (ret)
		goto failed_context;

	ret = mnt_context_umount(ctx);
	ret = mnt_context_get_excode(ctx, ret, buf, sizeof(buf));

failed_context:
	mnt_free_context(ctx);
failed:
	context->result = ret;
}

static void unmount_done(void *data)
{
	struct unmount_ctx *ctx = data;
	struct l_dbus_message *reply;

	if (ctx->result) {
		reply = dbus_error_failed(ctx->pending);
	} else {
		reply = l_dbus_message_new_method_return(ctx->pending);
		l_dbus_message_set_arguments(reply, "");
	}

	l_dbus_send(dbus_get_bus(), reply);
	l_dbus_message_unref(ctx->pending);

	l_free(ctx);
}

static struct l_dbus_message *filesystem_unmount(struct l_dbus *dbus,
						 struct l_dbus_message *message,
						 void *user_data)
{
	struct filesystem *fs = user_data;
	struct l_dbus_message_iter options;
	struct l_dbus_message_iter variant;
	struct unmount_ctx *ctx;
	const char *key;
	int ret;

	if (!l_dbus_message_get_arguments(message, "a{sv}", &options))
		return dbus_error_invalid_args(message);

	if (l_queue_isempty(fs->mountpoints))
		return dbus_error_not_mounted(message);
	if (fs->mount_pending)
		return dbus_error_busy(message);

	while (l_dbus_message_iter_next_entry(&options, &key, &variant)) {
		l_error("unsupported option to unmount: %s", key);
		return dbus_error_invalid_args(message);
	}

	ctx = l_new(struct unmount_ctx, 1);
	ctx->devnode = fs->devnode;
	ctx->pending = l_dbus_message_ref(message);
	ctx->mountpoint = l_queue_peek_tail(fs->mountpoints);
	ret = task_start(unmount_run, unmount_done, ctx);
	if (ret < 0) {
		l_dbus_message_unref(message);
		l_free(ctx);
		return dbus_error_from_errno(-ret, message);
	}

	return NULL;
}

static bool filesystem_property_get_type(struct l_dbus *dbus,
					 struct l_dbus_message *message,
					 struct l_dbus_message_builder *builder,
					 void *user_data)
{
	struct filesystem *fs = user_data;

	if (!fs->type)
		return false;

	return l_dbus_message_builder_append_basic(builder, 's', fs->type);
}

static bool filesystem_property_get_label(struct l_dbus *dbus,
					  struct l_dbus_message *message,
					  struct l_dbus_message_builder *builder,
					  void *user_data)
{
	struct filesystem *fs = user_data;

	if (!fs->label)
		return false;

	return l_dbus_message_builder_append_basic(builder, 's', fs->label);
}

static bool filesystem_property_get_uuid(struct l_dbus *dbus,
					 struct l_dbus_message *message,
					 struct l_dbus_message_builder *builder,
					 void *user_data)
{
	struct filesystem *fs = user_data;

	if (!fs->uuid)
		return false;

	return l_dbus_message_builder_append_basic(builder, 's', fs->uuid);
}

struct dbus_append_ctx {
	struct l_dbus_message_builder *builder;
	bool success;
};

static void filesystem_dbus_append_mountpoint(void *data, void *user_data)
{
	const char *mountpoint = data;
	struct dbus_append_ctx *ctx = user_data;

	ctx->success &= dbus_message_builder_append_basic_array(ctx->builder,
								'y', mountpoint,
								strlen(mountpoint) + 1);
}

static bool filesystem_property_get_mountpoints(struct l_dbus *dbus,
						struct l_dbus_message *message,
					 	struct l_dbus_message_builder *builder,
					 	void *user_data)
{
	struct filesystem *fs = user_data;
	struct dbus_append_ctx ctx = {
		.builder = builder,
		.success = true,
	};

	if (!l_dbus_message_builder_enter_array(builder, "ay"))
		return false;

	l_queue_foreach(fs->mountpoints, filesystem_dbus_append_mountpoint,
			&ctx);
	if (!ctx.success)
		return false;

	return l_dbus_message_builder_leave_array(builder);
}

static void filesystem_register_dbus(struct filesystem *fs)
{
	L_AUTO_FREE_VAR(char *, path) = block_device_get_path(fs->block);
	struct l_dbus *dbus = dbus_get_bus();

	/* Properties interface already added by block layer. */
	if (!l_dbus_object_add_interface(dbus, path, EDISKSD_FILESYSTEM_INTERFACE,
					 fs))
		l_warn("Unable to add %s interface", EDISKSD_FILESYSTEM_INTERFACE);
}

static void setup_filesystem_interface(struct l_dbus_interface *interface)
{
	l_dbus_interface_method(interface, "Mount", 0,
				filesystem_mount, "s", "a{sv}",
				"mountpoint", "options");
	l_dbus_interface_method(interface, "Unmount", 0,
				filesystem_unmount, "", "a{sv}",
				"options");

	l_dbus_interface_property(interface, "Type", 0, "s",
				  filesystem_property_get_type, NULL);
	l_dbus_interface_property(interface, "Label", 0, "s",
				  filesystem_property_get_label, NULL);
	l_dbus_interface_property(interface, "UUID", 0, "s",
				  filesystem_property_get_uuid, NULL);
	l_dbus_interface_property(interface, "Mountpoints", 0, "aay",
				  filesystem_property_get_mountpoints, NULL);
}

static void filesystem_free(struct filesystem *fs)
{
	if (!fs)
		return;

	l_free(fs->uuid);
	l_free(fs->label);
	l_free(fs->type);

	l_free(fs->mount_type);
	l_free(fs->mount_opts);

	l_free(fs);
}

void filesystem_mounted(dev_t dev, const char *mountpoint)
{
	struct filesystem *fs = filesystem_lookup_by_dev(dev);
	L_AUTO_FREE_VAR(char *, path) = NULL;

	l_info("mounted %u:%u at %s", major(dev), minor(dev), mountpoint);

	if (!fs)
		return;

	l_queue_push_tail(fs->mountpoints, l_strdup(mountpoint));

	path = block_device_get_path(fs->block);
	l_dbus_property_changed(dbus_get_bus(), path,
				EDISKSD_FILESYSTEM_INTERFACE,
				"Mountpoints");
}

static bool filesystem_match_mountpoint(const void *a, const void *b)
{
	return !strcmp(a, b);
}

void filesystem_unmounted(dev_t dev, const char *mountpoint)
{
	struct filesystem *fs = filesystem_lookup_by_dev(dev);
	L_AUTO_FREE_VAR(char *, path) = NULL;
	char *managed;

	l_info("unmounted %u:%u at %s", major(dev), minor(dev), mountpoint);

	managed = l_queue_remove_if(managed_mountpoints,
				    filesystem_match_mountpoint,
				    mountpoint);
	if (managed) {
		if (rmdir(managed) < 0)
			l_warn("failed to clean up mountpoint %s", managed);

		l_free(managed);
	}

	if (!fs)
		return;

	l_queue_remove_if(fs->mountpoints, filesystem_match_mountpoint,
			  mountpoint);

	path = block_device_get_path(fs->block);
	l_dbus_property_changed(dbus_get_bus(), path,
				EDISKSD_FILESYSTEM_INTERFACE,
				"Mountpoints");
}

static void unmount_stale_done(void *data)
{
	struct unmount_ctx *ctx = data;

	l_free((char *)ctx->devnode);
	l_free((char *)ctx->mountpoint);
	l_free(ctx);
}

static void filesystem_unmount_stale(void *data, void *user_data)
{
	char *mountpoint = data;
	struct filesystem *fs = user_data;
	struct unmount_ctx *ctx;

	ctx = l_new(struct unmount_ctx, 1);
	ctx->devnode = l_strdup(fs->devnode);
	ctx->mountpoint = mountpoint;

	if (task_start(unmount_run, unmount_stale_done, ctx) < 0) {
		l_free(ctx);
		l_free(mountpoint);
	}
}

static const char *get_device_property_or_empty(struct udev_device *device,
						const char *key)
{
	const char *ret;

	ret = udev_device_get_property_value(device, key);
	if (!ret)
		ret = "";

	return ret;
}

void filesystem_added(struct block_device *block, struct udev_device *device)
{
	struct filesystem *fs = l_new(struct filesystem, 1);

	fs->block = block;
	fs->type = l_strdup(get_device_property_or_empty(device, "ID_FS_TYPE"));
	fs->devnode = l_strdup(udev_device_get_devnode(device));
	fs->dev = block_device_get_dev(block);
	fs->label = l_strdup(get_device_property_or_empty(device, "ID_FS_LABEL"));
	fs->uuid = l_strdup(get_device_property_or_empty(device, "ID_FS_UUID"));
	fs->mountpoints = l_queue_new();

	l_queue_push_tail(filesystems, fs);

	filesystem_register_dbus(fs);
}

void filesystem_removed(struct block_device *block)
{
	struct filesystem *fs;

	fs = filesystem_lookup_by_dev(block_device_get_dev(block));
	if (!fs)
		return;

	l_queue_remove(filesystems, fs);
	l_queue_foreach(fs->mountpoints, filesystem_unmount_stale, fs);
	/* Ownership of mountpoint data is passed to unmount workers */
	l_queue_clear(fs->mountpoints, NULL);
	filesystem_free(fs);
}

static void filesystem_update_property(struct filesystem *fs, const char *prop,
				       char **value, const char *new_value)
{
	L_AUTO_FREE_VAR(char *, path) = NULL;

	if (!*value && !new_value)
		return;

	if (*value && new_value && !strcmp(*value, new_value))
		return;

	l_free(*value);
	*value = l_strdup(new_value);

	path = block_device_get_path(fs->block);
	l_dbus_property_changed(dbus_get_bus(), path,
				EDISKSD_FILESYSTEM_INTERFACE, prop);
}

void filesystem_changed(struct block_device *block, struct udev_device *device)
{
	struct filesystem *fs;

	fs = filesystem_lookup_by_dev(block_device_get_dev(block));
	if (!fs)
		return;

	filesystem_update_property(fs, "Type", &fs->type,
				   get_device_property_or_empty(device, "ID_FS_TYPE"));
	filesystem_update_property(fs, "Label", &fs->label,
				   get_device_property_or_empty(device, "ID_FS_LABEL"));
	filesystem_update_property(fs, "UUID", &fs->uuid,
				   get_device_property_or_empty(device, "ID_FS_UUID"));
}

int filesystem_init(void)
{
	struct l_dbus *dbus = dbus_get_bus();

	if (!l_dbus_register_interface(dbus, EDISKSD_FILESYSTEM_INTERFACE,
				       setup_filesystem_interface,
				       NULL, false)) {
		l_error("Unable to register %s interface",
			EDISKSD_FILESYSTEM_INTERFACE);
		return -EPERM;
	}

	filesystems = l_queue_new();
	managed_mountpoints = l_queue_new();

	return 0;
}

static void filesystem_destroy_cb(void *data)
{
	filesystem_free(data);
}

void filesystem_exit(void)
{
	struct l_dbus *dbus = dbus_get_bus();

	l_dbus_unregister_interface(dbus, EDISKSD_FILESYSTEM_INTERFACE);
	l_queue_destroy(filesystems, filesystem_destroy_cb);
}
