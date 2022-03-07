// SPDX-License-Identifier: LGPL-2.1+

#ifndef EDISKSD_CLIENT_H
#define EDISKSD_CLIENT_H

struct l_dbus_client;
struct l_dbus_proxy;

typedef void (*filesystem_cmd_t)(void);

void set_filesystem_cmd(struct l_dbus_client *client, filesystem_cmd_t cmd);

struct l_dbus_proxy *find_filesystem_for_device(const char *device);
struct l_dbus_proxy *find_filesystem_for_mountpoint(const char *mountpoint);

int monitor_main(struct l_dbus_client *client, int argc, char *argv[]);
int mount_main(struct l_dbus_client *clinet, int argc, char *argv[]);
int unmount_main(struct l_dbus_client *clinet, int argc, char *argv[]);

#endif
