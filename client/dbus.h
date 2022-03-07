// SPDX-License-Identifier: LGPL-2.1+

#ifndef CLIENT_DBUS_H
#define CLIENT_DBUS_H

#define EDISKSD_SERVICE			"io.github.inmusicdev.edisksd"
#define EDISKSD_BLOCK_INTERFACE		"io.github.inmusicdev.edisksd.Block"
#define EDISKSD_DAEMON_INTERFACE	"io.github.inmusicdev.edisksd.Daemon"
#define EDISKSD_FILESYSTEM_INTERFACE	"io.github.inmusicdev.edisksd.Filesystem"

#define EDISKSD_BASE_PATH		"/io/github/inmusicdev/edisksd"

bool dbus_proxy_get_byte_array_property(struct l_dbus_proxy *proxy,
					const char *name,
					void *value, uint32_t *n);

#endif

