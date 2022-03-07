// SPDX-License-Identifier: LGPL-2.1+
#ifndef FUSE_PROC_H
#define FUSE_PROC_H

struct fuse_operations;
struct fuse_pollhandle;

struct proc_file_info {
	const struct fuse_operations *ops;
	struct fuse_pollhandle *ph;
};

extern const struct fuse_operations mountinfo_ops;

#endif
