// SPDX-License-Identifier: LGPL-2.1+

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <ell/ell.h>

#include "src/decode.h"


static char hexval(char c)
{
	if ('0' <= c && c <= '9')
		return c - '0';
	if ('a' <= c && c <= 'f')
		return c - 'a' + 10;
	if ('A' <= c && c <= 'F')
		return c - 'A' + 10;

	return 0;
}

static char *validate_utf8(char *s)
{
	size_t len = strlen(s);
	const char *end;
	char *ret;

	if (l_utf8_validate(s, len, &end))
		return s;

	ret = l_strndup(s, end - s);
	l_free(s);

	return ret;
}

char *decode_blkid_string(const char *s)
{
	size_t len = strlen(s);
	struct l_string *ret = l_string_new(len);

	for (; *s; s++) {
		char c = *s;

		if (c == '\\') {
			if (s[1] != 'x' || !s[2] || !s[3])
				continue;

			c = (hexval(s[2]) << 4) | hexval(s[3]);
			s += 3;
		}

		l_string_append_c(ret, c);
	}

	return validate_utf8(l_string_unwrap(ret));
}
