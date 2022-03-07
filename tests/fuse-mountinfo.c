// SPDX-License-Identifier: LGPL-2.1+
#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <errno.h>

#include <sys/sysmacros.h>

#include <ell/ell.h>
#include <fuse.h>

#include "fuse-proc.h"


struct mount {
	struct mount *next;

	unsigned int flags;
#define MOUNTF_TOMBSTONE	(1U << 0)
	unsigned int id;

	struct mount *parent;
	dev_t dev;

	char *mnt_root;
	char *path;
	char *mnt_opts;
	char *type;
	char *devname;
	char *sb_opts;
};

struct mountinfo_file_info {
	struct proc_file_info base;
	off_t read_pos;

	unsigned int buf_offset;
	struct l_string *buf;

	struct mount *cursor;
};

static int n_active_files;
static struct mount mount_root = {
	.id = 1,
};

static struct mountinfo_file_info *mountinfo_fi(struct fuse_file_info *fi)
{
	return (struct mountinfo_file_info *) fi->fh;
}

static void mount_free_tombstones(void)
{
	struct mount **prev = &mount_root.next;

	while (*prev) {
		struct mount *next = (*prev)->next;

		if (next->flags & MOUNTF_TOMBSTONE) {
			*prev = next->next;

			free(next->mnt_root);
			free(next->path);
			free(next->mnt_opts);
			free(next->type);
			free(next->devname);
			free(next->sb_opts);
			free(next);
		}

		prev = &(*prev)->next;
	}
}

static int show_mountinfo(struct mount *mount, struct l_string *str)
{
	l_string_append_printf(str, "%i %i %u:%u ", mount->id, mount->parent->id,
			       major(mount->dev), minor(mount->dev));
	l_string_append(str, mount->mnt_root);
	l_string_append_c(str, ' ');
	l_string_append(str, mount->path);
	l_string_append_c(str, ' ');
	l_string_append(str, mount->mnt_opts);

	/* No support for key:value flags. */
	l_string_append(str, " - ");

	l_string_append(str, mount->type);
	l_string_append_c(str, ' ');
	l_string_append(str, mount->devname);
	l_string_append_c(str, ' ');
	l_string_append(str, mount->sb_opts);
	l_string_append_c(str, '\n');

	return l_string_length(str);
}

static int open_mountinfo(const char *filename, struct fuse_file_info *fi)
{
	struct mountinfo_file_info *mfi = calloc(1, sizeof (*mfi));

	if (!mfi)
		return -ENOMEM;

	mfi->base.ops = &mountinfo_ops;
	mfi->buf = l_string_new(4095);
	mfi->cursor = &mount_root;

	fi->direct_io = 1;
	fi->fh = (uintptr_t) mfi;

	return 0;
}

static int release_mountinfo(const char *filename, struct fuse_file_info *fi)
{
	struct mountinfo_file_info *mfi = mountinfo_fi(fi);

	l_string_free(mfi->buf);

	if (mfi->read_pos && --n_active_files < 1)
		mount_free_tombstones();

	return 0;
}

static int read_mountinfo(const char *filename, char *buf, size_t len,
			  off_t pos, struct fuse_file_info *fi)
{
	struct mountinfo_file_info *mfi = mountinfo_fi(fi);
	char *const buf_start = buf;
	int ret = 0;

	if (pos != mfi->read_pos) {
		if (mfi->read_pos && --n_active_files < 1)
			mount_free_tombstones();

		mfi->cursor = &mount_root;
		mfi->read_pos = 0;

		// FIXME: catch up
		if (pos)
			return -EINVAL; //mountinfo_catchup(mfi, pos);
	}

	if (mfi->buf_offset < l_string_length(mfi->buf)) {
		size_t to_copy = l_string_length(mfi->buf) - mfi->buf_offset;

		if (to_copy > len)
			to_copy = len;

		memcpy(buf, l_string_unwrap(mfi->buf) + mfi->buf_offset, to_copy);
		mfi->buf_offset += to_copy;
		buf += to_copy;
		len -= to_copy;
	}

	while (len) {
		l_string_truncate(mfi->buf, 0);

		mfi->cursor = mfi->cursor->next;
		while (mfi->cursor && mfi->cursor->flags & MOUNTF_TOMBSTONE)
			mfi->cursor = mfi->cursor->next;

		if (!mfi->cursor)
			break; /* EOF */

		ret = show_mountinfo(mfi->cursor, mfi->buf);
		if (ret < 0)
			break;

		mfi->buf_offset = len;
		if ((unsigned) ret > len)
			ret = len;

		memcpy(buf, l_string_unwrap(mfi->buf), ret);
		buf += ret;
		len -= ret;
	}

	if (ret < 0 && buf == buf_start)
		return ret;

	ret = (int) (buf - buf_start);

	if (!mfi->read_pos && ret)
		n_active_files++;

	mfi->read_pos += ret;
	return ret;
}

const struct fuse_operations mountinfo_ops = {
	.open = open_mountinfo,
	.release = release_mountinfo,
	.read = read_mountinfo,
};
