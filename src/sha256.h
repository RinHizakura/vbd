/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef VBD_SHA256_H
#define VBD_SHA256_H

#include <stddef.h>
#include <stdint.h>

#define SHA256_LEN	32

void sha256(const void *buf, size_t len, uint8_t out[SHA256_LEN]);

#endif
