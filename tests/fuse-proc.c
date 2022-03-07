// SPDX-License-Identifier: LGPL-2.1+
#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <err.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>

#include <ell/ell.h>
#include <fuse.h>
#include <fuse_lowlevel.h>

#include "fuse-proc.h"


struct proc_dentry {
	const char *name;
	mode_t mode;

	union {
		/* If S_IFDIR, null terminated array */
		const struct proc_dentry *children;
		/* If S_IFREG, must implement open() */
		const struct fuse_operations *ops;
	};
};

static struct fuse *fuse;
struct fuse_session *session;
static struct fuse_buf fuse_buf;

static struct proc_file_info *proc_fi(struct fuse_file_info *fi)
{
	return (struct proc_file_info *) fi->fh;
}

static const struct fuse_operations *proc_ops(struct fuse_file_info *fi)
{
	return proc_fi(fi)->ops;
}

static const struct proc_dentry proc_self_dentries[] = {
	{
		.mode = S_IFREG | 0444,
		.name = "mountinfo",
		.ops = &mountinfo_ops,
	},
	{ 0 }
};

static const struct proc_dentry proc_dentries[] = {
	{
		.mode = S_IFDIR | 0555,
		.name = "self",
		.children = proc_self_dentries,
	},
	{ 0 }
};

static const struct proc_dentry proc_root = {
	.mode = S_IFDIR | 0555,
	.name = "",
	.children = proc_dentries,
};

static int proc_find_dentry(const char *filename, const struct proc_dentry **out)
{
	const struct proc_dentry *dentry = &proc_root;
	char **cmpts;
	char **cmpt;
	int ret = 0;

	if (filename[0] == '/')
		filename++;

	cmpts = l_strsplit(filename, '/');
	cmpt = cmpts;

	while (*cmpt) {
		const char *name = *cmpt++;

		if (!S_ISDIR(dentry->mode))
			break;

		for (dentry = dentry->children; dentry->name; dentry++)
			if (!strcmp(dentry->name, name))
				break;

		if (!dentry->name)
			break;
	}

	if (*cmpt)
		ret = dentry->name ? -ENOTDIR : -ENOENT;

	l_strv_free(cmpts);

	if (!ret && out)
		*out = dentry;
	return ret;
}

static void proc_fill_stat(struct stat *st, const struct proc_dentry *dentry)
{
	st->st_mode = dentry->mode;

	clock_gettime(CLOCK_REALTIME, &st->st_atim);
	st->st_mtim = st->st_ctim = st->st_atim;
}

static int proc_getattr(const char *filename, struct stat *st,
			struct fuse_file_info *fi)
{
	const struct proc_dentry *dentry;
	int ret;

	ret = proc_find_dentry(filename, &dentry);
	if (ret)
		return ret;

	proc_fill_stat(st, dentry);
	return 0;
}

static int proc_open(const char *filename, struct fuse_file_info *fi)
{
	const struct proc_dentry *dentry;
	int ret;

	ret = proc_find_dentry(filename, &dentry);
	if (ret)
		return ret;

	if (S_ISDIR(dentry->mode))
		return -EISDIR;
	if (!S_ISREG(dentry->mode))
		return -EINVAL;

	if ((fi->flags & O_ACCMODE) != O_RDONLY)
		return -EROFS;

	return dentry->ops->open(filename, fi);
}

static int proc_read(const char *filename, char *buf, size_t len, off_t pos,
		     struct fuse_file_info *fi)
{
	const struct fuse_operations *ops = proc_ops(fi);

	if (!ops->read)
		return -ENOSYS;

	return ops->read(filename, buf, len, pos, fi);
}

static int proc_release(const char *filename, struct fuse_file_info *fi)
{
	const struct fuse_operations *ops = proc_ops(fi);
	struct proc_file_info *pfi = proc_fi(fi);

	if (ops->release)
		ops->release(filename, fi);

	fuse_pollhandle_destroy(pfi->ph);
	free(pfi);
	return 0;
}

static int proc_poll(const char *filename, struct fuse_file_info *fi,
		     struct fuse_pollhandle *ph, unsigned *revents)
{
	const struct fuse_operations *ops = proc_ops(fi);
	struct proc_file_info *pfi = proc_fi(fi);

	if (!ops->poll)
		return -ENOSYS;

	if (pfi->ph && pfi->ph != ph)
		fuse_pollhandle_destroy(ph);
	pfi->ph = ph;

	return ops->poll(filename, fi, ph, revents);
}

static int proc_readdir(const char *filename, void *buf,
			fuse_fill_dir_t filler, off_t pos,
			struct fuse_file_info *fi,
			enum fuse_readdir_flags flags)
{
	const struct proc_dentry *dentry;
	int ret;

	ret = proc_find_dentry(filename, &dentry);
	if (ret)
		return ret;

	if (!S_ISDIR(dentry->mode))
		return -ENOTDIR;

	for (dentry = &dentry->children[pos]; dentry->name; dentry++) {
		struct stat st = { 0 };

		proc_fill_stat(&st, dentry);
		if (filler(buf, dentry->name, &st, pos++, 0))
			break;
	}

	return 0;
}

static const struct fuse_operations operations = {
	.getattr = proc_getattr,

	.open = proc_open,
	.read = proc_read,
	.release = proc_release,
	.poll = proc_poll,

	.readdir = proc_readdir,
};

static bool fuse_read_cb(struct l_io *io, void *user_data)
{
	int ret;

	ret = fuse_session_receive_buf(session, &fuse_buf);
	if (ret == -EINTR)
		return true;
	if (ret <= 0)
		return false;

	fuse_session_process_buf(session, &fuse_buf);
	return !fuse_session_exited(session);
}

static void fuse_disconnect_cb(struct l_io *io, void *user_data)
{
	l_info("Filesystem unmounted, exiting");
	l_main_quit();
}

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

int main(int argc, char *argv[])
{
	const char *fuse_argv[] = {
		"test-procfs",
		"-oro",
		"-odefault_permissions",
		NULL
	};
	struct fuse_args args = {
		L_ARRAY_SIZE(fuse_argv) - 1,
		(char **) fuse_argv,
		0,
	};
	const char *mountpoint;
	struct l_io *fuse_io;
	int ret;

	if (!l_main_init())
		return EXIT_FAILURE;

	l_log_set_stderr();

	if (argc != 2)
		errx(1, "usage: fuse-proc <mountpoint>");

	mountpoint = argv[1];

	fuse = fuse_new(&args, &operations, sizeof(operations), NULL);
	if (!fuse)
		errx(1, "failed to create fuse object");

	session = fuse_get_session(fuse);
	if (fuse_mount(fuse, mountpoint))
		errx(1, "failed to mount fuse filesystem");

	fuse_io = l_io_new(fuse_session_fd(session));
	if (!fuse_io)
		errx(1, "failed to create io");

	if (!l_io_set_read_handler(fuse_io, fuse_read_cb, fuse, NULL))
		errx(1, "failed to configure fuse io");
	if (!l_io_set_disconnect_handler(fuse_io, fuse_disconnect_cb, fuse, NULL))
		errx(1, "failed to configure fuse io");

	ret = l_main_run_with_signal(signal_handler, NULL);

	l_io_destroy(fuse_io);
	fuse_unmount(fuse);
	fuse_destroy(fuse);

	free(fuse_buf.mem);
	l_main_exit();
	return ret;
}
