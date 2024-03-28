// SPDX-License-Identifier: LGPL-2.1+

#ifndef EDISKSD_H
#define EDISKSD_H

struct l_dbus;
struct l_settings;

const struct l_settings *edisksd_get_config(void);
struct l_dbus *dbus_get_bus(void);

#endif
