// SPDX-License-Identifier: LGPL-2.1+
#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <dlfcn.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>

#include <ell/ell.h>

struct mount_entry {
};

static struct l_queue *mount_list;
static FILE *(*real_fopen)(const char *, const char *);
static int (*real_open)(const char *, int, ...);

static __attribute__((constructor)) void init_mount_shim(void)
{
	real_fopen = dlsym(RTLD_NEXT, "fopen");
	real_open = dlsym(RTLD_NEXT, "open");
	if (!real_fopen || !real_open)
		abort();

	mount_list = l_queue_new();

	// FIXME: parse initial mounts
}

static void destroy_mount_entry(void *data)
{
	struct mount_entry *e = data;

	l_free(e);
}

static __attribute__((destructor)) void exit_mount_shim(void)
{
	l_queue_destroy(mount_list, destroy_mount_entry);
}

static ssize_t mountinfo_read(void *cookie, char *buf, size_t size)
{
	return 0;
}

static int mountinfo_seek(void *cookie, off64_t *offset, int whence)
{
	return 0;
}

static int mountinfo_close(void *cookie)
{
	return 0;
}

static const cookie_io_functions_t mountinfo_io_funcs = {
	.read = mountinfo_read,
	.seek = mountinfo_seek,
	.close = mountinfo_close,
};

#if 0
FILE *fopen(const char *restrict pathname, const char *restrict mode)
{
	if (!strcmp(pathname, "/proc/self/mountinfo")) {
		void *cookie = NULL;

		return fopencookie(cookie, mode, mountinfo_io_funcs);
	}

	return real_fopen(pathname, mode);
}
#endif

int open(const char *pathname, int flags, ...)
{
	mode_t mode = 0;

	if ((flags & O_CREAT) == O_CREAT || (flags & O_TMPFILE) == O_TMPFILE) {
		va_list ap;

		va_start(ap, flags);
		mode = va_arg(ap, mode_t);
		va_end(ap);
	}

	if (!strcmp(pathname, "/proc/self/mountinfo")) {
		return open("/dev/null", flags, mode);
	}

	return real_open(pathname, flags, mode);
}
