// SPDX-License-Identifier: LGPL-2.1+

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <ell/ell.h>

#include "client/dbus.h"


bool dbus_proxy_get_byte_array_property(struct l_dbus_proxy *proxy,
					const char *name,
					void *value, uint32_t *n)
{
	struct l_dbus_message_iter iter;

	if (!l_dbus_proxy_get_property(proxy, "Device", "ay", &iter))
		return false;

	return l_dbus_message_iter_get_fixed_array(&iter, value, n);
}
