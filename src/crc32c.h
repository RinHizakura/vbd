/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef VBD_CRC32C_H
#define VBD_CRC32C_H

#include <stddef.h>
#include <stdint.h>

uint32_t crc32c(uint32_t crc, const void *buf, size_t len);

#endif /* VBD_CRC32C_H */
