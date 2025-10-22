// SPDX-License-Identifier: LGPL-2.1+

#ifndef EDISKSD_DBUS_H
#define EDISKSD_DBUS_H

#define EDISKSD_SERVICE			"io.github.inmusicdev.edisksd"
#define EDISKSD_BLOCK_INTERFACE		"io.github.inmusicdev.edisksd.Block"
#define EDISKSD_DAEMON_INTERFACE	"io.github.inmusicdev.edisksd.Daemon"
#define EDISKSD_FILESYSTEM_INTERFACE	"io.github.inmusicdev.edisksd.Filesystem"

#define EDISKSD_BASE_PATH		"/io/github/inmusicdev/edisksd"

struct l_dbus_message;
struct l_dbus_message_builder;

bool dbus_message_builder_append_basic_array(struct l_dbus_message_builder *,
					     char type, const void *value,
					     size_t count);

struct l_dbus_message *dbus_error_failed(struct l_dbus_message *msg, const char *extra);
struct l_dbus_message *dbus_error_not_implemented(struct l_dbus_message *msg);
struct l_dbus_message *dbus_error_invalid_args(struct l_dbus_message *msg);
struct l_dbus_message *dbus_error_already_mounted(struct l_dbus_message *msg);
struct l_dbus_message *dbus_error_not_mounted(struct l_dbus_message *msg);
struct l_dbus_message *dbus_error_busy(struct l_dbus_message *msg);

struct l_dbus_message *dbus_error_from_errno(int err, struct l_dbus_message *msg);

#endif
