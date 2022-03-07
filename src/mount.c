// SPDX-License-Identifier: LGPL-2.1+

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <errno.h>
#include <stdio.h>

#include <sys/sysmacros.h>

#include <ell/ell.h>
#include <libmount.h>

#include "src/edisksd.h"
#include "src/filesystem.h"
#include "src/mount.h"

static struct libmnt_monitor *mount_monitor;
static struct l_io *mount_monitor_io;

static struct libmnt_table *mount_tables[2];
static struct libmnt_tabdiff *mount_diff;
static struct libmnt_iter *mount_iter;
static unsigned int table_index;

static void update_mountinfo(void)
{
	struct libmnt_table *old_table, *new_table;
	struct libmnt_fs *old_fs, *new_fs;
	int oper;

	table_index ^= 1;
	new_table = mount_tables[table_index];
	old_table = mount_tables[table_index ^ 1];

	if (mnt_reset_table(new_table) < 0) {
		l_warn("failed to reset mount table");
		return;
	}

	if (mnt_table_parse_file(new_table, "/proc/self/mountinfo") < 0) {
		l_warn("failed to parse mountinfo");
		return;
	}

	if (mnt_diff_tables(mount_diff, old_table, new_table) < 0) {
		l_warn("failed to diff mount tables");
		return;
	}

	mnt_reset_iter(mount_iter, MNT_ITER_FORWARD);
	while (mnt_tabdiff_next_change(mount_diff, mount_iter, &old_fs, &new_fs, &oper) == 0) {
		switch (oper) {
		case MNT_TABDIFF_MOUNT:
			l_debug("mount dev=%u:%u src=%s srcpath=%s target=%s",
				major(mnt_fs_get_devno(new_fs)), minor(mnt_fs_get_devno(new_fs)),
				mnt_fs_get_source(new_fs) ?: "<unknown>",
				mnt_fs_get_srcpath(new_fs) ?: "<unknown>",
				mnt_fs_get_target(new_fs) ?: "<unknown>");
			filesystem_mounted(mnt_fs_get_devno(new_fs),
					   mnt_fs_get_target(new_fs));
			break;

		case MNT_TABDIFF_UMOUNT:
			l_debug("unmount dev=%u:%u src=%s srcpath=%s target=%s",
				major(mnt_fs_get_devno(old_fs)), minor(mnt_fs_get_devno(old_fs)),
				mnt_fs_get_source(old_fs) ?: "<unknown>",
				mnt_fs_get_srcpath(old_fs) ?: "<unknown>",
				mnt_fs_get_target(old_fs) ?: "<unknown>");
			filesystem_unmounted(mnt_fs_get_devno(old_fs),
					     mnt_fs_get_target(old_fs));
			break;

		default:
			l_warn("unhandle mount table diff operation %d", oper);
			break;
		}
	}
}

static bool monitor_read(struct l_io *io, void *user_data)
{
	int ret;

	while ((ret = mnt_monitor_next_change(mount_monitor, NULL, NULL)) == 0)
		update_mountinfo();

	if (ret < 0)
		l_warn("failed to read from mount monitor: %d", ret);

	return true;
}

static bool mountinfo_setup_io(void)
{
	mount_monitor_io = l_io_new(mnt_monitor_get_fd(mount_monitor));
	if (!mount_monitor_io) {
		l_error("failed to configure monitor i/o");
		return false;
	}

	if (!l_io_set_read_handler(mount_monitor_io, monitor_read, NULL, NULL)) {
		l_error("failed to configure monitor i/o");
		return false;
	}

	return true;
}

int mount_init(void)
{
	int ret = -ENOMEM;

	mount_monitor = mnt_new_monitor();
	if (!mount_monitor) {
		l_error("failed to create mount monitor");
		return -ENOMEM;
	}

	mount_tables[0] = mnt_new_table();
	mount_tables[1] = mnt_new_table();
	if (!mount_tables[0] || !mount_tables[1]) {
		l_error("failed to create mount tables");
		goto failed_unref;
	}

	mount_diff = mnt_new_tabdiff();
	if (!mount_diff) {
		l_error("failed to create mount table diff");
		goto failed_unref;
	}

	mount_iter = mnt_new_iter(MNT_ITER_FORWARD);
	if (!mount_iter) {
		l_error("failed to create mount iterator");
		goto failed_unref;
	}

	ret = mnt_monitor_enable_kernel(mount_monitor, 1);
	if (ret < 0) {
		l_error("failed to enable mount table monitoring");
		goto failed_unref;
	}

	ret = -ENOMEM;
	if (!mountinfo_setup_io())
		goto failed_unref;

	return 0;

failed_unref:
	mnt_free_iter(mount_iter);
	mnt_free_tabdiff(mount_diff);
	mnt_unref_table(mount_tables[1]);
	mnt_unref_table(mount_tables[0]);
	mnt_unref_monitor(mount_monitor);
	return ret;
}

void mount_exit(void)
{
	l_io_destroy(mount_monitor_io);

	mnt_free_iter(mount_iter);
	mnt_free_tabdiff(mount_diff);
	mnt_unref_table(mount_tables[1]);
	mnt_unref_table(mount_tables[0]);
	mnt_unref_monitor(mount_monitor);
}
