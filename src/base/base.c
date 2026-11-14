// SPDX-License-Identifier: GPL-2.0-or-later
#include <string.h>

#include "base.h"

const struct base_ops *base_ops_for(const char *name)
{
	if (!strncmp(name, "http://", 7) || !strncmp(name, "https://", 8))
		return &base_http_ops;
	return &base_file_ops;
}
