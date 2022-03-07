// SPDX-License-Identifier: LGPL-2.1+

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <errno.h>

#include <ell/ell.h>

#include "src/dbus.h"


static int get_basic_size(const char type)
{
	switch (type) {
	case 'y':
		return 1;
	default:
		l_error("unsupported type: %c", type);
		return 0;
	}
}

bool dbus_message_builder_append_basic_array(struct l_dbus_message_builder *builder,
					     char type, const void *value,
					     size_t count)
{
	const char sig[] = { type, '\0' };
	const char *data = value;
	int size = get_basic_size(type);

	if (!l_dbus_message_builder_enter_array(builder, sig))
		return false;

	while (count--) {
		if (!l_dbus_message_builder_append_basic(builder, type, data))
			return false;

		data += size;
	}

	return l_dbus_message_builder_leave_array(builder);
}

struct l_dbus_message *dbus_error_failed(struct l_dbus_message *msg)
{
	return l_dbus_message_new_error(msg, EDISKSD_SERVICE ".Failed",
					"Operation failed");
}

struct l_dbus_message *dbus_error_not_implemented(struct l_dbus_message *msg)
{
	return l_dbus_message_new_error(msg, EDISKSD_SERVICE ".NotImplemented",
					"Not implemented");
}

struct l_dbus_message *dbus_error_invalid_args(struct l_dbus_message *msg)
{
	return l_dbus_message_new_error(msg, EDISKSD_SERVICE ".InvalidArgs",
					"Invalid arguments");
}

struct l_dbus_message *dbus_error_already_mounted(struct l_dbus_message *msg)
{
	return l_dbus_message_new_error(msg, EDISKSD_SERVICE ".AlreadyMounted",
					"Already mounted");
}

struct l_dbus_message *dbus_error_not_mounted(struct l_dbus_message *msg)
{
	return l_dbus_message_new_error(msg, EDISKSD_SERVICE ".NotMounted",
					"Not mounted");
}

struct l_dbus_message *dbus_error_busy(struct l_dbus_message *msg)
{
	return l_dbus_message_new_error(msg, EDISKSD_SERVICE ".InProgress",
					"Operation already in progress");
}

struct l_dbus_message *dbus_error_from_errno(int err, struct l_dbus_message *msg)
{
	switch (err) {
	case EBUSY:
		return dbus_error_busy(msg);
	case ENOSYS:
		return dbus_error_not_implemented(msg);
	default:
		break;
	}

	return dbus_error_failed(msg);
}
